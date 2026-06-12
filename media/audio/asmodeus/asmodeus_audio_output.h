// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Virtual audio output device backed by an shm ring buffer.
// Used by AsmodeusParticipant to give each hidden WebContents its own
// audio output path, separate from the real speakers.

#ifndef MEDIA_AUDIO_ASMODEUS_ASMODEUS_AUDIO_OUTPUT_H_
#define MEDIA_AUDIO_ASMODEUS_ASMODEUS_AUDIO_OUTPUT_H_

#include <memory>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/timer/timer.h"
#include "media/audio/asmodeus/audio_ring_buffer.h"
#include "media/audio/audio_io.h"
#include "media/base/audio_parameters.h"

namespace asmodeus {

// An AudioOutputStream that renders audio to an shm ring buffer instead
// of real speakers. The Pipecat agent reads from the shm for STT.
class AsmodeusAudioOutput : public media::AudioOutputStream {
 public:
  AsmodeusAudioOutput(const media::AudioParameters& params,
                      const std::string& shm_path);
  ~AsmodeusAudioOutput() override;

  // AudioOutputStream implementation.
  bool Open() override;
  void Start(AudioSourceCallback* callback) override;
  void Stop() override;
  void Close() override;
  void Flush() override;
  void SetVolume(double volume) override;
  void GetVolume(double* volume) override;

 private:
  void OnTimer();

  media::AudioParameters params_;
  std::string shm_path_;
  AudioRingBuffer ring_;       // Output: mixed audio (remote + local)
  AudioRingBuffer mic_ring_;   // Input: local mic (for mixing into output)
  AudioRingBuffer host_ring_;  // Host broadcast: all meeting audio
  raw_ptr<AudioSourceCallback> callback_ = nullptr;
  double volume_ = 1.0;
  std::unique_ptr<base::RepeatingTimer> timer_;
  std::vector<float> render_buffer_;
  std::vector<float> mic_buffer_;
  std::vector<float> host_buffer_;
  bool opened_ = false;
  bool mic_opened_ = false;
  bool host_opened_ = false;
  int log_counter_ = 0;
};

}  // namespace asmodeus

#endif  // MEDIA_AUDIO_ASMODEUS_ASMODEUS_AUDIO_OUTPUT_H_
