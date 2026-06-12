// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/audio/asmodeus/audio_ring_buffer.h"

#include <algorithm>
#include <cstring>

#include "base/logging.h"

#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "base/containers/span.h"
#include "base/compiler_specific.h"

namespace asmodeus {

AudioRingBuffer::AudioRingBuffer() = default;

AudioRingBuffer::~AudioRingBuffer() {
  Close();
}

bool AudioRingBuffer::Create(const std::string& path, int sample_rate,
                             int channels, size_t buffer_samples) {
  Close();
  path_ = path;
  buffer_samples_ = buffer_samples;
  mapped_size_ = kHeaderSize + buffer_samples * sizeof(float);

  fd_ = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0666);
  if (fd_ < 0) return false;

  if (ftruncate(fd_, static_cast<off_t>(mapped_size_)) != 0) {
    close(fd_);
    fd_ = -1;
    return false;
  }

  mapped_ = mmap(nullptr, mapped_size_, PROT_READ | PROT_WRITE,
                 MAP_SHARED, fd_, 0);
  if (mapped_ == MAP_FAILED) {
    mapped_ = nullptr;
    close(fd_);
    fd_ = -1;
    return false;
  }

  header_ = static_cast<Header*>(mapped_);
  // SAFETY: buffer_ points into mmap'd region after the header.
  // Size is validated by mapped_size_ = kHeaderSize + buffer_samples * sizeof(float).
  buffer_ = UNSAFE_BUFFERS(reinterpret_cast<float*>(
      static_cast<uint8_t*>(mapped_) + kHeaderSize));

  header_->sample_rate.store(static_cast<uint32_t>(sample_rate),
                             std::memory_order_relaxed);
  header_->channels.store(static_cast<uint32_t>(channels),
                          std::memory_order_relaxed);
  header_->write_pos.store(0, std::memory_order_relaxed);
  header_->read_pos.store(0, std::memory_order_relaxed);
  header_->write_timestamp_us.store(0, std::memory_order_relaxed);

  // SAFETY: buffer_ has buffer_samples floats allocated.
  UNSAFE_BUFFERS(std::fill(buffer_, buffer_ + buffer_samples, 0.0f));
  return true;
}

bool AudioRingBuffer::Open(const std::string& path) {
  Close();
  path_ = path;

  fd_ = open(path.c_str(), O_RDWR);
  if (fd_ < 0) {
    LOG(ERROR) << "[Asmodeus] AudioRingBuffer::Open failed: " << path
               << " errno=" << errno << " (" << strerror(errno) << ")";
    return false;
  }

  struct stat st;
  if (fstat(fd_, &st) != 0) {
    close(fd_);
    fd_ = -1;
    return false;
  }

  mapped_size_ = static_cast<size_t>(st.st_size);
  if (mapped_size_ <= kHeaderSize) {
    close(fd_);
    fd_ = -1;
    return false;
  }

  mapped_ = mmap(nullptr, mapped_size_, PROT_READ | PROT_WRITE,
                 MAP_SHARED, fd_, 0);
  if (mapped_ == MAP_FAILED) {
    mapped_ = nullptr;
    close(fd_);
    fd_ = -1;
    return false;
  }

  header_ = static_cast<Header*>(mapped_);
  // SAFETY: same reasoning as Create().
  buffer_ = UNSAFE_BUFFERS(reinterpret_cast<float*>(
      static_cast<uint8_t*>(mapped_) + kHeaderSize));
  buffer_samples_ = (mapped_size_ - kHeaderSize) / sizeof(float);
  return true;
}

bool AudioRingBuffer::CreateIfNeeded(const std::string& path,
                                      int sample_rate, int channels,
                                      size_t buffer_samples) {
  struct stat st;
  if (stat(path.c_str(), &st) == 0 &&
      static_cast<size_t>(st.st_size) > kHeaderSize) {
    return Open(path);
  }
  return Create(path, sample_rate, channels, buffer_samples);
}

void AudioRingBuffer::Close() {
  if (mapped_) {
    munmap(mapped_, mapped_size_);
    mapped_ = nullptr;
  }
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }
  header_ = nullptr;
  buffer_ = nullptr;
  buffer_samples_ = 0;
}

size_t AudioRingBuffer::Write(const float* samples, size_t count) {
  if (!header_ || !buffer_ || count == 0) return 0;

  uint32_t wp = header_->write_pos.load(std::memory_order_relaxed);

  // SAFETY: wp % buffer_samples_ is always < buffer_samples_, which is
  // the number of floats allocated in the mmap'd region after the header.
  for (size_t i = 0; i < count; ++i) {
    UNSAFE_BUFFERS(buffer_[wp % buffer_samples_] = samples[i]);
    ++wp;
  }

  header_->write_pos.store(wp, std::memory_order_release);
  // Timestamp for lip-sync correlation
  auto now = std::chrono::steady_clock::now().time_since_epoch();
  header_->write_timestamp_us.store(
      static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(now).count()),
      std::memory_order_release);
  return count;
}

size_t AudioRingBuffer::Read(float* samples, size_t count) {
  if (!header_ || !buffer_ || count == 0) return 0;

  uint32_t wp = header_->write_pos.load(std::memory_order_acquire);
  uint32_t rp = header_->read_pos.load(std::memory_order_relaxed);

  uint32_t available = wp - rp;
  if (available > static_cast<uint32_t>(buffer_samples_)) {
    rp = wp - static_cast<uint32_t>(buffer_samples_);
    available = static_cast<uint32_t>(buffer_samples_);
  }

  size_t to_read = std::min(static_cast<size_t>(available), count);

  // SAFETY: rp % buffer_samples_ is always < buffer_samples_.
  for (size_t i = 0; i < to_read; ++i) {
    UNSAFE_BUFFERS(samples[i] = buffer_[rp % buffer_samples_]);
    ++rp;
  }

  header_->read_pos.store(rp, std::memory_order_release);
  return to_read;
}

size_t AudioRingBuffer::ReadAt(float* samples, size_t count,
                               uint32_t* local_rp) {
  if (!header_ || !buffer_ || count == 0 || !local_rp) return 0;

  uint32_t wp = header_->write_pos.load(std::memory_order_acquire);
  uint32_t rp = *local_rp;

  uint32_t available = wp - rp;
  if (available > static_cast<uint32_t>(buffer_samples_)) {
    // Reader fell behind — skip to latest data
    rp = wp - static_cast<uint32_t>(buffer_samples_);
    available = static_cast<uint32_t>(buffer_samples_);
  }

  size_t to_read = std::min(static_cast<size_t>(available), count);

  // SAFETY: rp % buffer_samples_ is always < buffer_samples_.
  for (size_t i = 0; i < to_read; ++i) {
    UNSAFE_BUFFERS(samples[i] = buffer_[rp % buffer_samples_]);
    ++rp;
  }

  *local_rp = rp;
  // Do NOT update header_->read_pos — caller manages their own position.
  return to_read;
}

size_t AudioRingBuffer::Available() const {
  if (!header_) return 0;
  uint32_t wp = header_->write_pos.load(std::memory_order_acquire);
  uint32_t rp = header_->read_pos.load(std::memory_order_relaxed);
  uint32_t avail = wp - rp;
  if (avail > static_cast<uint32_t>(buffer_samples_)) {
    return buffer_samples_;
  }
  return avail;
}

size_t AudioRingBuffer::Capacity() const {
  return buffer_samples_;
}

int AudioRingBuffer::sample_rate() const {
  if (!header_) return 0;
  return static_cast<int>(header_->sample_rate.load(std::memory_order_relaxed));
}

}  // namespace asmodeus
