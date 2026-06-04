// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AURELIAN_MEDIA_AURELIAN_VIRTUAL_MIC_H_
#define AURELIAN_MEDIA_AURELIAN_VIRTUAL_MIC_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "base/timer/timer.h"

namespace aurelian {

// AurelianVirtualMic — the audio analog of AurelianVirtualCamera. It feeds the
// agent's TTS into the browser microphone (AURELIAN-DESIGN §12 / C-MEDIA). The
// honest seam: the fork's FakeAudioInputStream::ChooseSource reads a producer's
// PCM from a shared-memory ring buffer (the fork's existing
// `~/.asmodeus/audio-in.shm` SharedMemorySource). This producer drains the TTS
// frames Cicero's audio_sink writes into MediaSeam and writes them into that
// ring, so getUserMedia({audio}) carries the agent's voice as the mic.
//
// Ring layout (matched verbatim to the reader, FakeAudioInputStream):
//   bytes  0..3   sample_rate (uint32)
//   bytes  4..7   channels    (uint32)
//   bytes  8..11  write_pos   (uint32, atomic)   <- this producer advances it
//   bytes 12..15  read_pos    (uint32, atomic)   <- the reader advances it
//   bytes 16..23  timestamp   (uint64)
//   bytes 24..    float32 ring buffer
class AurelianVirtualMic {
 public:
  // The default ring path the fork's FakeAudioInputStream reads.
  static std::string DefaultShmPath();

  AurelianVirtualMic();
  ~AurelianVirtualMic();

  AurelianVirtualMic(const AurelianVirtualMic&) = delete;
  AurelianVirtualMic& operator=(const AurelianVirtualMic&) = delete;

  // Production: create the ring at the default path and start the drain pump.
  // Must run on a sequence with a running task runner (the UI thread at boot).
  void Start();

  // Create/zero the ring at `path` WITHOUT starting the timer (tests drive the
  // pump deterministically via PumpOnce). Returns false if the ring can't be
  // created. Exposed for tests.
  bool OpenRingForTesting(const std::string& path);

  // Drain every queued MediaSeam audio frame and append its float samples to
  // the ring, advancing write_pos. Idempotent when the queue is empty.
  void PumpOnce();

  bool active() const { return mapped_ != nullptr; }
  size_t buffer_samples() const { return buffer_samples_; }

 private:
  bool OpenRing(const std::string& path);
  void CloseRing();

  int fd_ = -1;
  void* mapped_ = nullptr;
  size_t mapped_size_ = 0;
  size_t buffer_samples_ = 0;
  base::RepeatingTimer pump_timer_;
};

}  // namespace aurelian

#endif  // AURELIAN_MEDIA_AURELIAN_VIRTUAL_MIC_H_
