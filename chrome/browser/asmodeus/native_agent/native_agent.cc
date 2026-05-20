// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/asmodeus/native_agent/native_agent.h"

#include <cmath>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "base/compiler_specific.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/threading/platform_thread.h"
#include "base/values.h"
#include "chrome/browser/asmodeus/native_agent/video_shm.h"
#include "third_party/libyuv/include/libyuv.h"
#include "third_party/skia/include/core/SkBitmap.h"

#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/audio_options.h"
#include "api/create_modular_peer_connection_factory.h"
#include "api/enable_media_with_defaults.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "rtc_base/thread.h"

namespace asmodeus {

// (Resample22050To48000 removed — TTS now handled by ConversationEngine)

NativeAgent::Config::Config() = default;
NativeAgent::Config::~Config() = default;
NativeAgent::Config::Config(const Config&) = default;
NativeAgent::Config& NativeAgent::Config::operator=(const Config&) = default;

NativeAgent::NativeAgent(const Config& config) : config_(config) {}

NativeAgent::~NativeAgent() {
  if (conversation_) {
    conversation_->Stop();
  }
  control_.Stop();
  signaling_.Disconnect();
  peer_manager_.reset();
  pc_factory_ = nullptr;
  adm_ = nullptr;
  video_source_ = nullptr;
  audio_buffer_.Close();
  audio_out_buffer_.Close();
  if (video_mapped_) {
    munmap(video_mapped_, video_mapped_size_);
    close(video_fd_);
  }
}

bool NativeAgent::Init() {
  start_time_ = base::TimeTicks::Now();
  main_runner_ = base::SequencedTaskRunner::GetCurrentDefault();

  if (!audio_buffer_.CreateIfNeeded(config_.audio_shm_path, 48000, 1, 48000 * 10)) {
    LOG(ERROR) << "[" << config_.name << "] Failed to open/create audio-in shm";
    return false;
  }
  if (!audio_out_buffer_.CreateIfNeeded(config_.audio_out_shm_path, 48000, 1, 48000 * 10)) {
    LOG(ERROR) << "[" << config_.name << "] Failed to open/create audio-out shm";
    return false;
  }

  uint32_t frame_size = config_.video_width * config_.video_height * 3 / 2;
  size_t total_size = sizeof(VideoShmHeader) + frame_size * 2;
  video_fd_ = open(config_.video_shm_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (video_fd_ < 0) return false;
  ftruncate(video_fd_, total_size);
  video_mapped_ = mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, video_fd_, 0);
  if (video_mapped_ == MAP_FAILED) { close(video_fd_); video_mapped_ = nullptr; return false; }
  video_mapped_size_ = total_size;
  auto* header = reinterpret_cast<VideoShmHeader*>(video_mapped_);
  header->width = config_.video_width; header->height = config_.video_height;
  header->frame_size = frame_size;
  header->current_buffer.store(0); header->frame_sequence.store(0);

  renderer_ = std::make_unique<AvatarRenderer>(config_.name, config_.video_width, config_.video_height);
  // Write initial idle frame to video shm.
  {
    SkBitmap idle = renderer_->Render(0.0f);
    auto* vh = reinterpret_cast<VideoShmHeader*>(video_mapped_);
    uint32_t fs = vh->frame_size;
    uint32_t wb = 1 - vh->current_buffer.load();
    uint8_t* base_ptr = UNSAFE_BUFFERS(
        reinterpret_cast<uint8_t*>(video_mapped_) + sizeof(VideoShmHeader));
    uint8_t* nv12 = UNSAFE_BUFFERS(base_ptr + wb * fs);
    libyuv::ARGBToNV12(
        reinterpret_cast<const uint8_t*>(idle.getPixels()),
        config_.video_width * 4, nv12, config_.video_width,
        UNSAFE_BUFFERS(nv12 + config_.video_width * config_.video_height),
        config_.video_width, config_.video_width, config_.video_height);
    vh->frame_sequence.fetch_add(1);
    vh->current_buffer.store(wb);
  }
  LOG(INFO) << "[" << config_.name << "] Agent initialized";

  if (!config_.signaling_host.empty() && config_.signaling_port > 0) {
    InitWebRTC();
    ConnectSignaling();
  }

  if (config_.control_port > 0) {
    auto handler = [this](const std::string& cmd, const std::string& params) {
      return HandleCommand(cmd, params);
    };
    if (!control_.Start(config_.control_port, handler)) return false;
    LOG(INFO) << "[" << config_.name << "] Control: ws://127.0.0.1:" << control_.port();
  }

  // Start conversation engine (AudioLoop reads audio-out shm).
  InitConversation();

  LOG(INFO) << "[" << config_.name << "] Agent ready";
  return true;
}

void NativeAgent::InitConversation() {
  std::string home = getenv("HOME") ? getenv("HOME") : "/tmp";
  ConversationEngine::Config cfg;
  cfg.sample_rate = 48000;
  cfg.silero_model = home + "/.asmodeus/models/silero_vad.onnx";
  cfg.whisper_model = home + "/.asmodeus/models/whisper/ggml-base.en.bin";
  cfg.piper_path = config_.piper_path;
  cfg.voice_model = config_.voice_model;
  cfg.my_name = config_.display_name;
  cfg.turn_policy = "llm";
  cfg.personality = "You are " + config_.display_name +
      ", a participant in a voice meeting. "
      "You hear what others say. After each utterance, decide: respond or stay silent. "
      "RULES: "
      "1) If someone addresses you by name or asks you a question: RESPOND. "
      "2) If someone addresses another participant by name: SILENT. "
      "3) If a general question is asked: RESPOND only if relevant. "
      "4) Never repeat what was just said. Add your own thought. "
      "5) ONE short sentence only. Natural speech, no formatting. "
      "FORMAT: Reply with exactly one of:\n"
      "RESPOND: [your one-sentence reply]\n"
      "SILENT: [brief reason]";
  const char* api_key = getenv("ANTHROPIC_API_KEY");
  if (api_key) cfg.api_key = api_key;
  const char* google_key = getenv("GOOGLE_API_KEY");
  if (google_key) cfg.google_api_key = google_key;

  ConversationEngine::Callbacks cb;
  cb.on_heard = [this](const std::string& text, int stt_ms) {
    LOG(INFO) << "[" << config_.name << "] Heard: " << text
              << " (" << stt_ms << "ms)";
    base::DictValue evt;
    evt.Set("event", "heard");
    evt.Set("text", text);
    evt.Set("sttMs", stt_ms);
    std::string json;
    base::JSONWriter::Write(evt, &json);
    control_.EmitEvent(json);
  };
  cb.on_speaking = [this](const std::string& text, const std::string& source) {
    LOG(INFO) << "[" << config_.name << "] Speaking: " << text;
    base::DictValue evt;
    evt.Set("event", "speech_started");
    evt.Set("text", text);
    evt.Set("source", source);
    std::string json;
    base::JSONWriter::Write(evt, &json);
    control_.EmitEvent(json);
  };
  cb.on_speech_ended = [this](const std::string& text, double duration_ms) {
    LOG(INFO) << "[" << config_.name << "] Speech ended: " << duration_ms << "ms";
    base::DictValue evt;
    evt.Set("event", "speech_ended");
    evt.Set("text", text);
    evt.Set("durationMs", duration_ms);
    std::string json;
    base::JSONWriter::Write(evt, &json);
    control_.EmitEvent(json);
  };
  cb.on_barge_in = [this]() {
    LOG(INFO) << "[" << config_.name << "] Barge-in!";
    base::DictValue evt;
    evt.Set("event", "barge_in");
    std::string json;
    base::JSONWriter::Write(evt, &json);
    control_.EmitEvent(json);
  };
  cb.on_error = [this](const std::string& err) {
    LOG(ERROR) << "[" << config_.name << "] ConversationEngine error: " << err;
    base::DictValue evt;
    evt.Set("event", "error");
    evt.Set("message", err);
    std::string json;
    base::JSONWriter::Write(evt, &json);
    control_.EmitEvent(json);
  };

  conversation_ = std::make_unique<ConversationEngine>(
      std::move(cfg), std::move(cb),
      &audio_buffer_,
      &audio_out_buffer_,
      renderer_.get(),
      video_mapped_);

  conversation_->Init();
  conversation_->Start();
}

void NativeAgent::Run() {
  running_ = true;
  base::RunLoop run_loop;
  quit_closure_ = run_loop.QuitClosure();
  run_loop.Run();
}

void NativeAgent::Shutdown() {
  running_ = false;
  if (main_runner_) {
    main_runner_->PostTask(FROM_HERE, base::BindOnce([](NativeAgent* self) {
      if (self->quit_closure_) std::move(self->quit_closure_).Run();
    }, base::Unretained(this)));
  }
}

void NativeAgent::SpeakSync(const std::string& text) {
  if (conversation_) {
    conversation_->Speak(text);
    // Wait for AudioLoop to pick up the speak queue item.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    // Wait for speech to complete.
    while (conversation_->state() == ConversationEngine::State::Speaking) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    // Extra wait for audio to flush through shm.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
}

std::string NativeAgent::HandleCommand(const std::string& command, const std::string& params_json) {
  auto parsed = base::JSONReader::Read(params_json, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) return R"({"ok":false,"error":"Invalid JSON"})";
  auto& dict = parsed->GetDict();
  base::DictValue response;
  const auto* id_val = dict.Find("id");
  if (id_val) response.Set("id", id_val->Clone());

  if (command == "speak") {
    const std::string* text = dict.FindString("text");
    if (!text || text->empty()) {
      response.Set("ok", false); response.Set("error", "Missing text");
      std::string j; base::JSONWriter::Write(response, &j); return j;
    }
    if (conversation_) {
      conversation_->Speak(*text);
    }
    response.Set("ok", true); response.Set("queued", true);
    std::string j; base::JSONWriter::Write(response, &j); return j;
  }
  if (command == "get_state") {
    double uptime = (base::TimeTicks::Now() - start_time_).InMillisecondsF();
    base::DictValue state;
    state.Set("name", config_.name); state.Set("displayName", config_.display_name);
    state.Set("uptimeMs", static_cast<int>(uptime));
    if (conversation_) {
      state.Set("speaking", conversation_->state() == ConversationEngine::State::Speaking);
      auto cs = conversation_->state();
      const char* fsm = cs == ConversationEngine::State::Idle ? "idle"
          : cs == ConversationEngine::State::Listening ? "listening"
          : cs == ConversationEngine::State::Thinking ? "thinking"
          : "speaking";
      state.Set("fsmState", fsm);
      state.Set("rmsIn", static_cast<double>(conversation_->input_rms()));
    }
    response.Set("ok", true); response.Set("state", std::move(state));
    std::string j; base::JSONWriter::Write(response, &j); return j;
  }
  if (command == "shutdown") {
    Shutdown();
    response.Set("ok", true);
    std::string j; base::JSONWriter::Write(response, &j); return j;
  }
  response.Set("ok", false); response.Set("error", "Unknown command: " + command);
  std::string j; base::JSONWriter::Write(response, &j); return j;
}

bool NativeAgent::InitWebRTC() {
  network_thread_ = webrtc::Thread::CreateWithSocketServer();
  network_thread_->SetName("Net", nullptr); network_thread_->Start();
  worker_thread_ = webrtc::Thread::Create();
  worker_thread_->SetName("Worker", nullptr); worker_thread_->Start();
  rtc_signaling_thread_ = webrtc::Thread::Create();
  rtc_signaling_thread_->SetName("RtcSig", nullptr); rtc_signaling_thread_->Start();
  adm_ = ShmAudioDeviceModule::Create(config_.audio_shm_path,
                                      config_.audio_out_shm_path, 48000);
  video_source_ = webrtc::make_ref_counted<ShmVideoSource>(
      config_.video_shm_path, config_.video_width, config_.video_height, 30);
  webrtc::PeerConnectionFactoryDependencies deps;
  deps.network_thread = network_thread_.get(); deps.worker_thread = worker_thread_.get();
  deps.signaling_thread = rtc_signaling_thread_.get(); deps.adm = adm_;
  deps.audio_encoder_factory = webrtc::CreateBuiltinAudioEncoderFactory();
  deps.audio_decoder_factory = webrtc::CreateBuiltinAudioDecoderFactory();
  deps.video_encoder_factory = webrtc::CreateBuiltinVideoEncoderFactory();
  deps.video_decoder_factory = webrtc::CreateBuiltinVideoDecoderFactory();
  webrtc::EnableMediaWithDefaults(deps);
  pc_factory_ = webrtc::CreateModularPeerConnectionFactory(std::move(deps));
  if (!pc_factory_) { LOG(ERROR) << "[" << config_.name << "] PCF failed"; return false; }
  LOG(INFO) << "[" << config_.name << "] WebRTC initialized";
  video_source_->Start(); adm_->Init(); adm_->StartRecording();
  return true;
}

bool NativeAgent::ConnectSignaling() {
  signaling_.on_welcome = [this](const std::string& id, std::vector<std::string> peers) {
    main_runner_->PostTask(FROM_HERE, base::BindOnce(&NativeAgent::OnSignalingWelcome,
        base::Unretained(this), id, std::move(peers)));
  };
  signaling_.on_peer_joined = [this](const std::string& pid) {
    main_runner_->PostTask(FROM_HERE, base::BindOnce(&NativeAgent::OnSignalingPeerJoined,
        base::Unretained(this), pid));
  };
  signaling_.on_peer_left = [this](const std::string& pid) {
    main_runner_->PostTask(FROM_HERE, base::BindOnce(&NativeAgent::OnSignalingPeerLeft,
        base::Unretained(this), pid));
  };
  signaling_.on_peer_name = [this](const std::string& pid, const std::string& n) {
    LOG(INFO) << "[" << config_.name << "] Peer name: " << pid << " = " << n;
    main_runner_->PostTask(FROM_HERE, base::BindOnce([](NativeAgent* self,
        std::string peer_id, std::string name) {
      self->peer_names_[peer_id] = name;
      self->UpdateParticipantList();
    }, base::Unretained(this), pid, n));
  };
  signaling_.on_offer = [this](const std::string& f, const std::string& s) {
    main_runner_->PostTask(FROM_HERE, base::BindOnce(&NativeAgent::OnSignalingOffer,
        base::Unretained(this), f, s));
  };
  signaling_.on_answer = [this](const std::string& f, const std::string& s) {
    main_runner_->PostTask(FROM_HERE, base::BindOnce(&NativeAgent::OnSignalingAnswer,
        base::Unretained(this), f, s));
  };
  signaling_.on_ice_candidate = [this](const std::string& f, const std::string& c) {
    main_runner_->PostTask(FROM_HERE, base::BindOnce(&NativeAgent::OnSignalingIceCandidate,
        base::Unretained(this), f, c));
  };
  if (!signaling_.Connect(config_.signaling_host, config_.signaling_port, config_.display_name)) {
    LOG(ERROR) << "[" << config_.name << "] Signaling failed"; return false;
  }
  LOG(INFO) << "[" << config_.name << "] Signaling connected, id=" << signaling_.my_id();
  return true;
}

void NativeAgent::OnSignalingWelcome(const std::string& id, std::vector<std::string> peers) {
  LOG(INFO) << "[" << config_.name << "] Welcome: id=" << id << " peers=" << peers.size();
  if (pc_factory_ && !peer_manager_) {
    auto as = pc_factory_->CreateAudioSource(webrtc::AudioOptions());
    peer_manager_ = std::make_unique<WebRTCPeerManager>(pc_factory_, as, video_source_, &signaling_);
    for (const auto& p : peers) { if (id < p) peer_manager_->OnPeerJoined(p, true); }
  }
}

void NativeAgent::OnSignalingPeerJoined(const std::string& peer_id) {
  LOG(INFO) << "[" << config_.name << "] Peer joined: " << peer_id;
  if (peer_manager_) peer_manager_->OnPeerJoined(peer_id, signaling_.my_id() < peer_id);
}

void NativeAgent::OnSignalingPeerLeft(const std::string& peer_id) {
  LOG(INFO) << "[" << config_.name << "] Peer left: " << peer_id;
  if (peer_manager_) peer_manager_->OnPeerLeft(peer_id);
  peer_names_.erase(peer_id);
  UpdateParticipantList();
}

void NativeAgent::UpdateParticipantList() {
  if (!conversation_) return;
  std::vector<std::string> names;
  names.push_back(config_.display_name);  // include self
  for (const auto& [pid, name] : peer_names_) {
    names.push_back(name);
  }
  conversation_->SetParticipants(names);
  LOG(INFO) << "[" << config_.name << "] Participants: " << names.size();
}

void NativeAgent::OnSignalingOffer(const std::string& from, const std::string& sdp) {
  if (peer_manager_) peer_manager_->OnOffer(from, sdp);
}

void NativeAgent::OnSignalingAnswer(const std::string& from, const std::string& sdp) {
  if (peer_manager_) peer_manager_->OnAnswer(from, sdp);
}

void NativeAgent::OnSignalingIceCandidate(const std::string& from, const std::string& cand) {
  if (peer_manager_) peer_manager_->OnIceCandidate(from, cand);
}

}  // namespace asmodeus
