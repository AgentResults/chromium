// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_AUDIO_ASMODEUS_ASMODEUS_AUDIO_OUTPUT_TAP_H_
#define MEDIA_AUDIO_ASMODEUS_ASMODEUS_AUDIO_OUTPUT_TAP_H_

#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "media/audio/asmodeus/audio_ring_buffer.h"
#include "media/audio/audio_io.h"

namespace asmodeus {

// Wraps an AudioOutputStream to copy rendered audio to ring buffers.
//
// Host tap mode: wraps the default speaker output. The host's speakers play
// ALL meeting audio (all participants via WebRTC). This tap captures it and
// BROADCASTS to every participant's shm file. This is the reliable path —
// the host always gets all audio, so participants always hear everyone.
//
// Per-participant tap mode: wraps a dedicated AUHAL output for one
// participant. Captures whatever audio Chrome's mixer provides to this
// participant's output stream. Less reliable but provides per-participant
// isolation when it works.
class AsmodeusAudioOutputTap : public media::AudioOutputStream,
                               public media::AudioOutputStream::AudioSourceCallback {
 public:
  // Host tap constructor — also broadcasts to participant shm files
  AsmodeusAudioOutputTap(media::AudioOutputStream* wrapped,
                         AudioRingBuffer* output_buffer);

  // Per-participant tap with gain and ownership
  AsmodeusAudioOutputTap(media::AudioOutputStream* wrapped,
                         AudioRingBuffer* output_buffer,
                         bool owns_buffer,
                         float gain);

  ~AsmodeusAudioOutputTap() override;

  AsmodeusAudioOutputTap(const AsmodeusAudioOutputTap&) = delete;
  AsmodeusAudioOutputTap& operator=(const AsmodeusAudioOutputTap&) = delete;

  // AudioOutputStream — delegates to wrapped stream
  bool Open() override;
  void Start(AudioSourceCallback* callback) override;
  void Stop() override;
  void Close() override;
  void Flush() override;
  void SetVolume(double volume) override;
  void GetVolume(double* volume) override;

  // AudioSourceCallback — intercepts audio data
  int OnMoreData(base::TimeDelta delay,
                 base::TimeTicks delay_timestamp,
                 const media::AudioGlitchInfo& glitch_info,
                 media::AudioBus* dest) override;
  void OnError(ErrorType type) override;

 private:
  // Scan ~/.asmodeus/ for participant shm files to broadcast to
  void ScanParticipantBuffers();

  RAW_PTR_EXCLUSION media::AudioOutputStream* wrapped_;
  RAW_PTR_EXCLUSION AudioRingBuffer* output_buffer_;
  RAW_PTR_EXCLUSION AudioSourceCallback* real_callback_ = nullptr;
  bool owns_buffer_ = false;
  float gain_ = 1.0f;

  // Participant shm buffers for broadcast (host tap only)
  std::vector<AudioRingBuffer*> participant_buffers_;

  int total_calls_ = 0;
  int nonzero_calls_ = 0;
  bool is_active_ = false;
  float prev_rms_ = 0.0f;
  std::string skip_name_;
};

// Initialize the persistent broadcast system for host tap broadcasting.
// Called once by AudioManagerMac when ~/.asmodeus/ exists.
void InitPersistentBroadcast(const std::string& host_shm_path);

}  // namespace asmodeus

#endif  // MEDIA_AUDIO_ASMODEUS_ASMODEUS_AUDIO_OUTPUT_TAP_H_
