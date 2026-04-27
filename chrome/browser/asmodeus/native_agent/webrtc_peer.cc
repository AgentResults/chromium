// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/asmodeus/native_agent/webrtc_peer.h"

#include "base/compiler_specific.h"
#include "base/json/json_reader.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "chrome/browser/asmodeus/native_agent/signaling_client.h"

#include "api/jsep.h"
#include "api/peer_connection_interface.h"
#include "api/rtc_error.h"

namespace asmodeus {

// Observer for a single PeerConnection
class WebRTCPeerManager::PeerObserver
    : public webrtc::PeerConnectionObserver {
 public:
  PeerObserver(const std::string& peer_id, WebRTCPeerManager* manager)
      : peer_id_(peer_id), manager_(manager) {}

  void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override {}
  void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface>) override {}
  void OnRenegotiationNeeded() override {}

  void OnIceConnectionChange(
      webrtc::PeerConnectionInterface::IceConnectionState state) override {
    LOG(INFO) << "ICE state for " << peer_id_ << ": "
              << static_cast<int>(state);
  }

  void OnIceGatheringChange(
      webrtc::PeerConnectionInterface::IceGatheringState) override {}

  void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override {
    std::string sdp;
    candidate->ToString(&sdp);
    std::string json = "{\"type\":\"ice-candidate\",\"to\":\"" + peer_id_ +
        "\",\"candidate\":{\"candidate\":\"" + sdp +
        "\",\"sdpMid\":\"" + candidate->sdp_mid() +
        "\",\"sdpMLineIndex\":" +
        std::to_string(candidate->sdp_mline_index()) + "}}";
    manager_->signaling_->Send(json);
  }

  void OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) override {
    auto track = transceiver->receiver()->track();
    LOG(INFO) << "Received " << track->kind() << " track from " << peer_id_;
  }

  void OnAddStream(webrtc::scoped_refptr<webrtc::MediaStreamInterface>) override {}
  void OnRemoveStream(webrtc::scoped_refptr<webrtc::MediaStreamInterface>) override {}

 private:
  std::string peer_id_;
  RAW_PTR_EXCLUSION WebRTCPeerManager* manager_;
};

// Observer for SetLocalDescription / SetRemoteDescription
class WebRTCPeerManager::SetSDPObserver
    : public webrtc::SetSessionDescriptionObserver {
 public:
  static webrtc::scoped_refptr<SetSDPObserver> Create(const std::string& op) {
    return webrtc::make_ref_counted<SetSDPObserver>(op);
  }
  explicit SetSDPObserver(const std::string& op) : op_(op) {}
  void OnSuccess() override {
    LOG(INFO) << "SetSDP success: " << op_;
  }
  void OnFailure(webrtc::RTCError error) override {
    LOG(ERROR) << "SetSDP failed: " << op_ << " - " << error.message();
  }
 private:
  std::string op_;
};

// Observer for CreateOffer / CreateAnswer
class WebRTCPeerManager::CreateSDPObserver
    : public webrtc::CreateSessionDescriptionObserver {
 public:
  static webrtc::scoped_refptr<CreateSDPObserver> Create(
      const std::string& peer_id,
      webrtc::PeerConnectionInterface* pc,
      SignalingClient* signaling,
      bool is_offer) {
    return webrtc::make_ref_counted<CreateSDPObserver>(
        peer_id, pc, signaling, is_offer);
  }

  CreateSDPObserver(const std::string& peer_id,
                     webrtc::PeerConnectionInterface* pc,
                     SignalingClient* signaling,
                     bool is_offer)
      : peer_id_(peer_id), pc_(pc), signaling_(signaling),
        is_offer_(is_offer) {}

  void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
    std::string sdp;
    desc->ToString(&sdp);

    // Set local description with the created SDP
    pc_->SetLocalDescription(
        SetSDPObserver::Create("setLocal-" + peer_id_).get(),
        desc);  // Takes ownership

    std::string type = is_offer_ ? "offer" : "answer";
    std::string json = "{\"type\":\"" + type + "\",\"to\":\"" + peer_id_ +
        "\",\"sdp\":{\"type\":\"" + type + "\",\"sdp\":\"";
    // Escape newlines in SDP
    for (char c : sdp) {
      if (c == '\n') json += "\\n";
      else if (c == '\r') json += "\\r";
      else if (c == '"') json += "\\\"";
      else if (c == '\\') json += "\\\\";
      else json += c;
    }
    json += "\"}}";

    signaling_->Send(json);
    LOG(INFO) << "Sent " << type << " to " << peer_id_;
  }

  void OnFailure(webrtc::RTCError error) override {
    LOG(ERROR) << "CreateSDP failed for " << peer_id_
               << ": " << error.message();
  }

 private:
  std::string peer_id_;
  RAW_PTR_EXCLUSION webrtc::PeerConnectionInterface* pc_;
  RAW_PTR_EXCLUSION SignalingClient* signaling_;
  bool is_offer_;
};

WebRTCPeerManager::PeerInfo::PeerInfo() = default;
WebRTCPeerManager::PeerInfo::~PeerInfo() = default;
WebRTCPeerManager::PeerInfo::PeerInfo(PeerInfo&&) = default;
WebRTCPeerManager::PeerInfo& WebRTCPeerManager::PeerInfo::operator=(PeerInfo&&) = default;

// WebRTCPeerManager implementation

WebRTCPeerManager::WebRTCPeerManager(
    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory,
    webrtc::scoped_refptr<webrtc::AudioSourceInterface> audio_source,
    webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> video_source,
    SignalingClient* signaling)
    : factory_(std::move(factory)),
      audio_source_(std::move(audio_source)),
      video_source_(std::move(video_source)),
      signaling_(signaling) {}

WebRTCPeerManager::~WebRTCPeerManager() {
  for (auto& [id, info] : peers_) {
    if (info.pc) info.pc->Close();
  }
  peers_.clear();
}

void WebRTCPeerManager::CreatePeerConnection(const std::string& peer_id,
                                               bool create_offer) {
  if (peers_.count(peer_id)) return;

  auto observer = std::make_unique<PeerObserver>(peer_id, this);

  webrtc::PeerConnectionInterface::RTCConfiguration config;
  webrtc::PeerConnectionInterface::IceServer stun;
  stun.uri = "stun:stun.l.google.com:19302";
  config.servers.push_back(stun);

  webrtc::PeerConnectionDependencies deps(observer.get());
  auto result = factory_->CreatePeerConnectionOrError(config, std::move(deps));
  if (!result.ok()) {
    LOG(ERROR) << "Failed to create PeerConnection for " << peer_id
               << ": " << result.error().message();
    return;
  }
  auto pc = result.MoveValue();

  // Add audio track
  auto audio_track = factory_->CreateAudioTrack("audio", audio_source_.get());
  auto audio_result = pc->AddTrack(audio_track, {"stream"});
  if (!audio_result.ok()) {
    LOG(ERROR) << "Failed to add audio track: " << audio_result.error().message();
  }

  // Add video track
  auto video_track = factory_->CreateVideoTrack(video_source_, "video");
  auto video_result = pc->AddTrack(video_track, {"stream"});
  if (!video_result.ok()) {
    LOG(ERROR) << "Failed to add video track: " << video_result.error().message();
  }

  PeerInfo info;
  info.peer_id = peer_id;
  info.pc = pc;
  info.observer = std::move(observer);
  peers_[peer_id] = std::move(info);

  LOG(INFO) << "PeerConnection created for " << peer_id
            << (create_offer ? " (initiator)" : " (answerer)");

  if (create_offer) {
    pc->CreateOffer(
        CreateSDPObserver::Create(peer_id, pc.get(), signaling_, true).get(),
        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
  }
}

void WebRTCPeerManager::OnPeerJoined(const std::string& peer_id,
                                       bool is_initiator) {
  CreatePeerConnection(peer_id, is_initiator);
}

void WebRTCPeerManager::OnPeerLeft(const std::string& peer_id) {
  auto it = peers_.find(peer_id);
  if (it != peers_.end()) {
    if (it->second.pc) it->second.pc->Close();
    peers_.erase(it);
    LOG(INFO) << "PeerConnection closed for " << peer_id;
  }
}

void WebRTCPeerManager::OnOffer(const std::string& from,
                                  const std::string& sdp_json) {
  // Parse the SDP from the signaling message
  auto parsed = base::JSONReader::Read(sdp_json, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) return;
  auto& msg = parsed->GetDict();
  const auto* sdp_obj = msg.FindDict("sdp");
  if (!sdp_obj) return;
  const std::string* sdp_str = sdp_obj->FindString("sdp");
  if (!sdp_str) return;

  // Create PeerConnection if needed
  if (!peers_.count(from)) {
    CreatePeerConnection(from, false);
  }
  auto& info = peers_[from];

  // Set remote description (offer)
  webrtc::SdpParseError error;
  auto desc = webrtc::CreateSessionDescription(
      webrtc::SdpType::kOffer, *sdp_str, &error);
  if (!desc) {
    LOG(ERROR) << "Failed to parse offer SDP: " << error.description;
    return;
  }
  info.pc->SetRemoteDescription(
      SetSDPObserver::Create("setRemoteOffer-" + from).get(),
      desc.release());

  // Create answer
  info.pc->CreateAnswer(
      CreateSDPObserver::Create(from, info.pc.get(), signaling_, false).get(),
      webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
}

void WebRTCPeerManager::OnAnswer(const std::string& from,
                                   const std::string& sdp_json) {
  auto it = peers_.find(from);
  if (it == peers_.end()) return;

  auto parsed = base::JSONReader::Read(sdp_json, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) return;
  auto& msg = parsed->GetDict();
  const auto* sdp_obj = msg.FindDict("sdp");
  if (!sdp_obj) return;
  const std::string* sdp_str = sdp_obj->FindString("sdp");
  if (!sdp_str) return;

  webrtc::SdpParseError error;
  auto desc = webrtc::CreateSessionDescription(
      webrtc::SdpType::kAnswer, *sdp_str, &error);
  if (!desc) {
    LOG(ERROR) << "Failed to parse answer SDP: " << error.description;
    return;
  }
  it->second.pc->SetRemoteDescription(
      SetSDPObserver::Create("setRemoteAnswer-" + from).get(),
      desc.release());
}

void WebRTCPeerManager::OnIceCandidate(const std::string& from,
                                         const std::string& candidate_json) {
  auto it = peers_.find(from);
  if (it == peers_.end()) return;

  auto parsed = base::JSONReader::Read(candidate_json, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) return;
  auto& msg = parsed->GetDict();
  const auto* cand_obj = msg.FindDict("candidate");
  if (!cand_obj) return;
  const std::string* cand_str = cand_obj->FindString("candidate");
  const std::string* sdp_mid = cand_obj->FindString("sdpMid");
  auto sdp_mline_index = cand_obj->FindInt("sdpMLineIndex");
  if (!cand_str) return;

  webrtc::SdpParseError error;
  std::unique_ptr<webrtc::IceCandidateInterface> candidate(
      webrtc::CreateIceCandidate(
          sdp_mid ? *sdp_mid : "",
          sdp_mline_index.value_or(0),
          *cand_str, &error));
  if (!candidate) {
    LOG(ERROR) << "Failed to parse ICE candidate: " << error.description;
    return;
  }
  if (!it->second.pc->AddIceCandidate(candidate.get())) {
    LOG(ERROR) << "AddIceCandidate failed for " << from;
  }
}

}  // namespace asmodeus
