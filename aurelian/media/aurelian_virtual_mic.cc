// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/media/aurelian_virtual_mic.h"

#include <cerrno>

#include "base/logging.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "aurelian/handles/media/media_seam.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"

namespace aurelian {

namespace {
constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kChannels = 1;
constexpr size_t kHeaderSize = 24;  // sr(4) ch(4) wp(4) rp(4) ts(8)
constexpr size_t kRingSamples = 96000;  // ~2s @ 48k mono

std::atomic<uint32_t>* WritePosAt(void* base) {
  return reinterpret_cast<std::atomic<uint32_t>*>(static_cast<uint8_t*>(base) +
                                                  8);
}
float* RingAt(void* base) {
  return reinterpret_cast<float*>(static_cast<uint8_t*>(base) + kHeaderSize);
}
}  // namespace

// static
std::string AurelianVirtualMic::DefaultShmPath() {
  const char* home = std::getenv("HOME");
  // The reader (FakeAudioInputStream) uses ~/.asmodeus/ because /tmp is blocked
  // by the macOS sandbox; match it exactly.
  return std::string(home ? home : "/tmp") + "/.asmodeus/audio-in.shm";
}

AurelianVirtualMic::AurelianVirtualMic() = default;

AurelianVirtualMic::~AurelianVirtualMic() {
  CloseRing();
}

void AurelianVirtualMic::Start() {
  const std::string path = DefaultShmPath();
  if (!OpenRing(path)) {
    // NEVER silent. The browser runs with device-count=0 so that
    // getUserMedia({audio}) resolves to THIS mic; if the ring does not open,
    // there is no audio input at all and every getUserMedia hangs forever with
    // no diagnostic. A device that failed to start must SAY SO.
    LOG(ERROR) << "[aurelian] virtual mic FAILED to open its ring at " << path
               << " (errno=" << errno << ") — getUserMedia({audio}) will never "
                  "resolve, because device-count=0 leaves no other input";
    return;
  }
  // Drain TTS into the ring at audio cadence. The reader pulls from read_pos on
  // the capture thread; this single producer advances write_pos.
  pump_timer_.Start(FROM_HERE, base::Milliseconds(10), this,
                    &AurelianVirtualMic::PumpOnce);
}

bool AurelianVirtualMic::OpenRingForTesting(const std::string& path) {
  return OpenRing(path);
}

bool AurelianVirtualMic::OpenRing(const std::string& path) {
  CloseRing();
  base::FilePath file_path(path);
  base::CreateDirectory(file_path.DirName());

  fd_ = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
  if (fd_ < 0) {
    return false;
  }
  mapped_size_ = kHeaderSize + kRingSamples * sizeof(float);
  if (ftruncate(fd_, static_cast<off_t>(mapped_size_)) != 0) {
    CloseRing();
    return false;
  }
  mapped_ = mmap(nullptr, mapped_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_,
                 0);
  if (mapped_ == MAP_FAILED) {
    mapped_ = nullptr;
    CloseRing();
    return false;
  }
  buffer_samples_ = kRingSamples;

  // Header: sample_rate, channels, write_pos=0, read_pos=0, timestamp=0.
  uint8_t* base = static_cast<uint8_t*>(mapped_);
  uint32_t sr = kSampleRate;
  uint32_t ch = kChannels;
  std::memcpy(base + 0, &sr, sizeof(sr));
  std::memcpy(base + 4, &ch, sizeof(ch));
  WritePosAt(mapped_)->store(0, std::memory_order_relaxed);
  reinterpret_cast<std::atomic<uint32_t>*>(base + 12)->store(
      0, std::memory_order_relaxed);
  uint64_t ts = 0;
  std::memcpy(base + 16, &ts, sizeof(ts));
  return true;
}

void AurelianVirtualMic::CloseRing() {
  pump_timer_.Stop();
  if (mapped_) {
    munmap(mapped_, mapped_size_);
    mapped_ = nullptr;
  }
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }
  buffer_samples_ = 0;
}

void AurelianVirtualMic::PumpOnce() {
  if (!mapped_ || buffer_samples_ == 0) {
    return;
  }
  std::vector<std::vector<uint8_t>> frames = MediaSeam::Get().DrainAudio();
  if (frames.empty()) {
    return;
  }
  std::atomic<uint32_t>* write_pos = WritePosAt(mapped_);
  float* ring = RingAt(mapped_);
  uint32_t wp = write_pos->load(std::memory_order_relaxed);
  for (const std::vector<uint8_t>& frame : frames) {
    const size_t n = frame.size() / sizeof(float);
    for (size_t i = 0; i < n; ++i) {
      float sample = 0.0f;
      std::memcpy(&sample, frame.data() + i * sizeof(float), sizeof(float));
      ring[wp % buffer_samples_] = sample;
      ++wp;
    }
  }
  // Publish the new write position to the reader (capture thread).
  write_pos->store(wp, std::memory_order_release);
}

}  // namespace aurelian
