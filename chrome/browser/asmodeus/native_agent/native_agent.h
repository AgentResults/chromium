// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ASMODEUS_NATIVE_AGENT_NATIVE_AGENT_H_
#define CHROME_BROWSER_ASMODEUS_NATIVE_AGENT_NATIVE_AGENT_H_

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "base/memory/raw_ptr_exclusion.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "chrome/browser/asmodeus/native_agent/avatar_renderer.h"
#include "chrome/browser/asmodeus/native_agent/control_channel.h"
#include "chrome/browser/asmodeus/native_agent/conversation_engine.h"
#include "chrome/browser/asmodeus/native_agent/shm_audio_device_module.h"
#include "chrome/browser/asmodeus/native_agent/shm_video_source.h"
#include "chrome/browser/asmodeus/native_agent/signaling_client.h"
#include "chrome/browser/asmodeus/native_agent/webrtc_peer.h"
#include "media/audio/asmodeus/audio_ring_buffer.h"

#include "api/peer_connection_interface.h"

namespace asmodeus {

// The native meeting agent. Owns all state, manages lifecycle.
//
// Thread model:
// - main_runner_: owns all agent state (peer_manager_, speaking_, etc.)
// - Signaling IO thread: receives messages, PostTasks to main_runner_
// - Control IO thread: receives commands, calls HandleCommand
// - TTS runs on base::ThreadPool for non-blocking speak
class NativeAgent {
 public:
  struct Config {
    Config();
    ~Config();
    Config(const Config&);
    Config& operator=(const Config&);
    std::string name;
    std::string display_name;
    std::string voice_model;
    std::string piper_path;
    std::string audio_shm_path;       // audio-in (our TTS output)
    std::string audio_out_shm_path;   // audio-out (received peer audio)
    std::string video_shm_path;
    int video_width = 640;
    int video_height = 480;
    int control_port = 0;
    std::string signaling_host;
    int signaling_port = 0;
  };

  explicit NativeAgent(const Config& config);
  ~NativeAgent();

  NativeAgent(const NativeAgent&) = delete;
  NativeAgent& operator=(const NativeAgent&) = delete;

  // Initialize all subsystems. Returns false on failure.
  bool Init();

  // Run the main loop (uses base::RunLoop). Blocks until shutdown.
  void Run();

  // Request shutdown (can be called from any thread / signal handler).
  void Shutdown();

  // Speak text (delegates to ConversationEngine). Used by --speak flag.
  void SpeakSync(const std::string& text);

 private:
  // Command handler for control channel (called on control IO thread).
  std::string HandleCommand(const std::string& command,
                            const std::string& params_json);

  // Set up signaling callbacks (PostTask to main_runner_) and connect.
  bool ConnectSignaling();

  // Set up WebRTC factory.
  bool InitWebRTC();

  // Signaling callbacks — posted to main_runner_ from IO thread.
  void OnSignalingWelcome(const std::string& id, std::vector<std::string> peers);
  void OnSignalingPeerJoined(const std::string& peer_id);
  void OnSignalingPeerLeft(const std::string& peer_id);
  void OnSignalingOffer(const std::string& from, const std::string& sdp);
  void OnSignalingAnswer(const std::string& from, const std::string& sdp);
  void OnSignalingIceCandidate(const std::string& from, const std::string& cand);

  Config config_;
  std::atomic<bool> running_{false};

  // Main thread task runner — all agent state mutations happen here.
  scoped_refptr<base::SequencedTaskRunner> main_runner_;
  // Quit closure for Run() loop.
  base::OnceClosure quit_closure_;

  // SHM
  AudioRingBuffer audio_buffer_;       // audio-in (our TTS output)
  AudioRingBuffer audio_out_buffer_;   // audio-out (received peer audio)
  int video_fd_ = -1;
  RAW_PTR_EXCLUSION void* video_mapped_ = nullptr;
  size_t video_mapped_size_ = 0;

  // Avatar
  std::unique_ptr<AvatarRenderer> renderer_;

  // Control channel
  ControlChannel control_;

  // Signaling
  SignalingClient signaling_;

  // WebRTC
  std::unique_ptr<webrtc::Thread> network_thread_;
  std::unique_ptr<webrtc::Thread> worker_thread_;
  std::unique_ptr<webrtc::Thread> rtc_signaling_thread_;
  webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> pc_factory_;
  webrtc::scoped_refptr<ShmAudioDeviceModule> adm_;
  webrtc::scoped_refptr<ShmVideoSource> video_source_;
  std::unique_ptr<WebRTCPeerManager> peer_manager_;

  base::TimeTicks start_time_;

  // Conversation engine (AudioLoop + VAD + AEC + STT + LLM).
  std::unique_ptr<ConversationEngine> conversation_;
  void InitConversation();

  // Peer name tracking for participant list.
  std::map<std::string, std::string> peer_names_;  // peer_id → display_name
  void UpdateParticipantList();
};

}  // namespace asmodeus

#endif  // CHROME_BROWSER_ASMODEUS_NATIVE_AGENT_NATIVE_AGENT_H_
