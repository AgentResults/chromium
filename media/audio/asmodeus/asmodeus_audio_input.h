// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_AUDIO_ASMODEUS_ASMODEUS_AUDIO_INPUT_H_
#define MEDIA_AUDIO_ASMODEUS_ASMODEUS_AUDIO_INPUT_H_

#include <memory>

#include "base/memory/raw_ptr_exclusion.h"
#include "base/timer/timer.h"
#include "media/audio/asmodeus/audio_ring_buffer.h"
#include "media/audio/audio_io.h"
#include "media/base/audio_bus.h"
#include "media/base/audio_parameters.h"

// Forward declare to avoid circular deps
namespace base { class Thread; }

namespace base {
class Thread;
}

namespace asmodeus {

// Virtual microphone backed by a shared memory ring buffer.
// Reads TTS audio from per-participant SHM and delivers it to Chrome's
// audio capture pipeline for WebRTC encoding.
//
// Takes either a raw pointer (caller owns) or a raw pointer that this class
// owns (allocated with new by AudioManagerMac). When created by
// MakeLowLatencyInputStream, AudioManagerMac passes a new'd AudioRingBuffer
// and this class takes ownership via the owned_ring_buffer_ member.
class AsmodeusAudioInput : public media::AudioInputStream {
 public:
  // Non-owning constructor (caller manages ring buffer lifetime).
  AsmodeusAudioInput(AudioRingBuffer* ring_buffer,
                     const media::AudioParameters& params);
  ~AsmodeusAudioInput() override;

  // Transfer ownership of the ring buffer to this input stream.
  void TakeOwnership(AudioRingBuffer* ring_buffer);

  AsmodeusAudioInput(const AsmodeusAudioInput&) = delete;
  AsmodeusAudioInput& operator=(const AsmodeusAudioInput&) = delete;

  OpenOutcome Open() override;
  void Start(AudioInputCallback* callback) override;
  void Stop() override;
  void Close() override;
  double GetMaxVolume() override;
  void SetVolume(double volume) override;
  double GetVolume() override;
  bool SetAutomaticGainControl(bool enabled) override;
  bool GetAutomaticGainControl() override;
  bool IsMuted() override;
  void SetOutputDeviceForAec(const std::string& output_device_id) override;

 private:
  void OnTimer();

  RAW_PTR_EXCLUSION AudioRingBuffer* ring_buffer_;
  std::unique_ptr<AudioRingBuffer> owned_ring_buffer_;  // Owns ring if TakeOwnership called
  media::AudioParameters params_;
  RAW_PTR_EXCLUSION AudioInputCallback* callback_ = nullptr;
  std::unique_ptr<base::Thread> capture_thread_;
  std::unique_ptr<base::RepeatingTimer> timer_;
  std::unique_ptr<media::AudioBus> audio_bus_;
  bool opened_ = false;
  double volume_ = 1.0;
  uint32_t local_read_pos_ = 0;  // Per-instance read position (not shared)
  uint64_t total_frames_ = 0;
  uint64_t nonzero_frames_ = 0;
  float prev_input_rms_ = 0.0f;
};

}  // namespace asmodeus

#endif  // MEDIA_AUDIO_ASMODEUS_ASMODEUS_AUDIO_INPUT_H_
