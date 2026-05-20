// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ASMODEUS_AUDIO_RING_BUFFER_H_
#define CHROME_BROWSER_ASMODEUS_AUDIO_RING_BUFFER_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "base/memory/raw_ptr_exclusion.h"

namespace asmodeus {

// Lock-free single-producer single-consumer ring buffer backed by
// a memory-mapped file for IPC with external processes.
//
// Layout in shared memory:
//   [0..3]   uint32  sample_rate
//   [4..7]   uint32  channels
//   [8..11]  uint32  write_pos (updated by producer)
//   [12..15] uint32  read_pos (updated by consumer)
//   [16..]   float32 samples (ring buffer)
//
// The buffer holds `buffer_samples` worth of float samples.
// When the writer overtakes the reader, old data is lost.
class AudioRingBuffer {
 public:
  // Header stored at the start of the shared memory region.
  struct Header {
    std::atomic<uint32_t> sample_rate;
    std::atomic<uint32_t> channels;
    std::atomic<uint32_t> write_pos;
    std::atomic<uint32_t> read_pos;
  };

  AudioRingBuffer();
  ~AudioRingBuffer();

  AudioRingBuffer(const AudioRingBuffer&) = delete;
  AudioRingBuffer& operator=(const AudioRingBuffer&) = delete;

  // Create a new shared memory file at `path` with the given capacity.
  // Returns true on success.
  bool Create(const std::string& path, int sample_rate, int channels,
              size_t buffer_samples);

  // Open an existing shared memory file for reading/writing.
  bool Open(const std::string& path);

  // Open if the file exists and is valid, otherwise Create.
  // Use this when the caller doesn't know whether another process
  // already created the file (e.g., coordinator pre-created it).
  bool CreateIfNeeded(const std::string& path, int sample_rate, int channels,
                      size_t buffer_samples);

  // Close and unmap.
  void Close();

  // Write `count` samples to the buffer. Returns number actually written.
  // If the buffer is full, wraps and overwrites oldest data.
  size_t Write(const float* samples, size_t count);

  // Read up to `count` samples from the buffer. Returns number actually read.
  size_t Read(float* samples, size_t count);

  // Number of samples available to read.
  size_t Available() const;

  // Total capacity in samples.
  size_t Capacity() const;

  bool is_open() const { return mapped_ != nullptr; }
  std::string path() const { return path_; }

 private:
  // RAW_PTR_EXCLUSION: pointers into mmap'd shared memory.
  RAW_PTR_EXCLUSION Header* header_ = nullptr;
  RAW_PTR_EXCLUSION float* buffer_ = nullptr;
  size_t buffer_samples_ = 0;
  RAW_PTR_EXCLUSION void* mapped_ = nullptr;
  size_t mapped_size_ = 0;
  int fd_ = -1;
  std::string path_;
};

}  // namespace asmodeus

#endif  // CHROME_BROWSER_ASMODEUS_AUDIO_RING_BUFFER_H_
