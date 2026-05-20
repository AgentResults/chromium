// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ASMODEUS_NATIVE_AGENT_CONVERSATION_ENGINE_H_
#define CHROME_BROWSER_ASMODEUS_NATIVE_AGENT_CONVERSATION_ENGINE_H_

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "chrome/browser/asmodeus/native_agent/dialogue_manager.h"
#include "chrome/browser/asmodeus/native_agent/speech_output.h"
#include "chrome/browser/asmodeus/native_agent/turn_policy.h"
#include "media/audio/asmodeus/audio_ring_buffer.h"

namespace asmodeus {

class AvatarRenderer;

// Orchestrator: connects AudioPipeline → DialogueManager → SpeechOutput.
// Manages threading, state machine, barge-in, scripted speak queue.
class ConversationEngine {
 public:
  struct Config {
    Config();
    ~Config();
    Config(const Config&);
    Config& operator=(const Config&);
    Config(Config&&);
    Config& operator=(Config&&);

    // Audio pipeline.
    std::string whisper_model;
    std::string silero_model;
    float vad_threshold = 0.5f;
    int min_speech_ms = 100;
    int min_silence_ms = 150;
    int sample_rate = 48000;

    // Dialogue.
    std::string personality;
    std::string api_key;
    std::string google_api_key;
    int max_tokens = 100;
    int max_history = 20;
    std::string turn_policy = "llm";
    std::string my_name;

    // Speech output.
    std::string piper_path;
    std::string voice_model;
  };

  struct Callbacks {
    Callbacks();
    ~Callbacks();
    Callbacks(const Callbacks&);
    Callbacks& operator=(const Callbacks&);
    Callbacks(Callbacks&&);
    Callbacks& operator=(Callbacks&&);

    std::function<void(const std::string& text, int stt_ms)> on_heard;
    std::function<void(const std::string& text, const std::string& source)>
        on_speaking;
    std::function<void(const std::string& text, double duration_ms)>
        on_speech_ended;
    std::function<void()> on_barge_in;
    std::function<void(const std::string& error)> on_error;
  };

  enum class State { Idle, Listening, Thinking, Speaking };

  ConversationEngine(Config config,
                     Callbacks callbacks,
                     AudioRingBuffer* audio_in_shm,
                     AudioRingBuffer* audio_out_shm,
                     AvatarRenderer* avatar_renderer,
                     void* video_shm_mapped);
  ~ConversationEngine();

  ConversationEngine(const ConversationEngine&) = delete;
  ConversationEngine& operator=(const ConversationEngine&) = delete;

  bool Init();
  void Start();
  void Stop();

  void Speak(const std::string& text);
  void CancelSpeech();

  void SetParticipants(const std::vector<std::string>& names);
  void SetPersonality(const std::string& personality);
  void OnSpeakerUpdate(const std::string& speaker, bool speaking);

  State state() const { return state_.load(); }
  float input_rms() const { return input_rms_.load(); }
  size_t transcript_size() const;

 private:
  void AudioLoop();
  void OnUtterance(const std::string& text, int stt_ms, int64_t timestamp_ms);
  void OnBargeIn();
  void SpeechWorkerFn(std::string text, bool is_scripted);

  Config config_;
  Callbacks callbacks_;
  std::atomic<State> state_{State::Idle};
  std::atomic<bool> running_{false};
  std::atomic<bool> cancel_{false};
  std::atomic<float> input_rms_{0.0f};

  // Layer 2: Dialogue.
  std::unique_ptr<DialogueManager> dialogue_;

  // Layer 3: Speech output.
  std::unique_ptr<SpeechOutput> speech_;

  // Layer 1: Audio pipeline (inline — AEC, VAD, STT state).
  // AudioPipeline is not yet extracted into its own class.
  // The AudioLoop, AEC, VAD, STT code lives here for now.
  // TODO: extract into audio_pipeline.h/cc in a future refactoring pass.
  RAW_PTR_EXCLUSION AudioRingBuffer* audio_out_shm_;
  RAW_PTR_EXCLUSION void* aec_state_ = nullptr;
  std::mutex aec_mu_;
  std::vector<int16_t> aec_playback_buf_;
  std::vector<int16_t> aec_rec_i16_;
  std::vector<int16_t> aec_out_i16_;
  struct VadImpl;
  std::unique_ptr<VadImpl> vad_impl_;
  bool vad_in_speech_ = false;
  int vad_consec_speech_ = 0;
  int vad_consec_silence_ = 0;
  float vad_last_prob_ = 0.0f;
  RAW_PTR_EXCLUSION void* whisper_ctx_ = nullptr;
  std::vector<float> stt_buffer_;

  enum class VadEvent { None, SpeechStart, SpeechEnd };
  VadEvent FeedVad(const float* samples_16k, int n);
  void AecInit();
  void AecDestroy();
  void AecCapture(const float* rec_16k, float* out_16k, int n);
  void AecPlayback(const float* ref_16k, int n);
  bool SttInit();
  void SttDestroy();
  void SttFeed(const float* samples_16k, int n);
  std::string SttTranscribe();
  void SttReset();

  uint64_t last_voice_frame_ = 0;
  uint64_t silence_threshold_ = 0;  // Computed per-agent from name hash
  std::thread audio_thread_;
  std::thread speech_worker_;

  std::mutex speak_mu_;
  std::queue<std::string> speak_queue_;
};

}  // namespace asmodeus

#endif  // CHROME_BROWSER_ASMODEUS_NATIVE_AGENT_CONVERSATION_ENGINE_H_
