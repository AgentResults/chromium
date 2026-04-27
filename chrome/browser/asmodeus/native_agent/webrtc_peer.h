// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ASMODEUS_NATIVE_AGENT_WEBRTC_PEER_H_
#define CHROME_BROWSER_ASMODEUS_NATIVE_AGENT_WEBRTC_PEER_H_

#include <functional>
#include <map>
#include <string>

#include "api/peer_connection_interface.h"
#include "api/scoped_refptr.h"
#include "base/memory/raw_ptr.h"

namespace asmodeus {

class SignalingClient;

// Manages WebRTC PeerConnections for the native agent.
// Creates PeerConnections, handles offer/answer/ICE exchange via signaling.
class WebRTCPeerManager {
 public:
  WebRTCPeerManager(
      webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory,
      webrtc::scoped_refptr<webrtc::AudioSourceInterface> audio_source,
      webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> video_source,
      SignalingClient* signaling);
  ~WebRTCPeerManager();

  // Called when a new peer is discovered (from signaling welcome or peer-joined)
  void OnPeerJoined(const std::string& peer_id, bool is_initiator);

  // Called when a peer leaves
  void OnPeerLeft(const std::string& peer_id);

  // Called when signaling messages arrive
  void OnOffer(const std::string& from, const std::string& sdp_json);
  void OnAnswer(const std::string& from, const std::string& sdp_json);
  void OnIceCandidate(const std::string& from, const std::string& candidate_json);

  int peer_count() const { return peers_.size(); }

 private:
  class PeerObserver;
  class SetSDPObserver;
  class CreateSDPObserver;

  struct PeerInfo {
    PeerInfo();
    ~PeerInfo();
    PeerInfo(PeerInfo&&);
    PeerInfo& operator=(PeerInfo&&);
    std::string peer_id;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
    std::unique_ptr<PeerObserver> observer;
  };

  void CreatePeerConnection(const std::string& peer_id, bool create_offer);

  webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory_;
  webrtc::scoped_refptr<webrtc::AudioSourceInterface> audio_source_;
  webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> video_source_;
  raw_ptr<SignalingClient> signaling_;  // not owned
  std::map<std::string, PeerInfo> peers_;
};

}  // namespace asmodeus

#endif  // CHROME_BROWSER_ASMODEUS_NATIVE_AGENT_WEBRTC_PEER_H_
