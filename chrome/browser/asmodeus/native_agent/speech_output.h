// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ASMODEUS_NATIVE_AGENT_SPEECH_OUTPUT_H_
#define CHROME_BROWSER_ASMODEUS_NATIVE_AGENT_SPEECH_OUTPUT_H_

#include <atomic>
#include <functional>
#include <string>

#include "base/memory/raw_ptr_exclusion.h"
#include "media/audio/asmodeus/audio_ring_buffer.h"

namespace asmodeus {

class AvatarRenderer;

class SpeechOutput {
 public:
  struct Config {
    Config();
    ~Config();
    Config(const Config&);
    Config& operator=(const Config&);

    std::string piper_path;
    std::string voice_model;
    float length_scale = 0.85f;
  };

  using OnAecReference = std::function<void(const float* samples_16k, int n)>;
  using OnSpeechStarted = std::function<void(const std::string& text)>;
  using OnSpeechEnded = std::function<void(const std::string& text, double ms)>;

  SpeechOutput(Config config,
               AudioRingBuffer* audio_in_shm,
               AvatarRenderer* avatar_renderer,
               void* video_shm_mapped,
               OnAecReference on_aec_ref,
               OnSpeechStarted on_started,
               OnSpeechEnded on_ended);
  ~SpeechOutput();

  SpeechOutput(const SpeechOutput&) = delete;
  SpeechOutput& operator=(const SpeechOutput&) = delete;

  // Speak text via TTS. BLOCKS until complete.
  // Must be called from SpeechWorker thread, NOT AudioLoop.
  void Speak(const std::string& text);

  // Cancel current speech. Thread-safe (called from AudioLoop).
  void Cancel();

  bool is_speaking() const { return speaking_.load(); }

 private:
  Config config_;
  OnAecReference on_aec_ref_;
  OnSpeechStarted on_started_;
  OnSpeechEnded on_ended_;

  RAW_PTR_EXCLUSION AudioRingBuffer* audio_in_shm_;
  RAW_PTR_EXCLUSION AvatarRenderer* avatar_renderer_;
  RAW_PTR_EXCLUSION void* video_shm_mapped_;

  std::atomic<bool> speaking_{false};
  std::atomic<bool> cancel_{false};
};

}  // namespace asmodeus

#endif  // CHROME_BROWSER_ASMODEUS_NATIVE_AGENT_SPEECH_OUTPUT_H_
