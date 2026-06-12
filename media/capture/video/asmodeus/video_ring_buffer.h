// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_CAPTURE_VIDEO_ASMODEUS_VIDEO_RING_BUFFER_H_
#define MEDIA_CAPTURE_VIDEO_ASMODEUS_VIDEO_RING_BUFFER_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "base/compiler_specific.h"
#include "base/memory/raw_ptr_exclusion.h"

namespace asmodeus {

// Lock-free video shared memory buffer.
//
// Layout matches the native agent's VideoShmHeader (video_shm.h):
//   Header (20 bytes): width, height, frame_size, current_buffer, frame_sequence
//   Frame data: 2 × frame_size bytes (double-buffered NV12 frames)
//
// Writer (native agent) renders avatar frames as NV12 and toggles current_buffer.
// Reader (Chrome) reads the latest frame from buffer[current_buffer].
class VideoRingBuffer {
 public:
  // Must match native_agent/video_shm.h VideoShmHeader exactly.
  struct Header {
    uint32_t width;
    uint32_t height;
    uint32_t frame_size;  // width * height * 3 / 2 for NV12
    std::atomic<uint32_t> current_buffer;  // 0 or 1
    std::atomic<uint32_t> frame_sequence;  // incremented on each new frame
  };

  VideoRingBuffer();
  ~VideoRingBuffer();

  VideoRingBuffer(const VideoRingBuffer&) = delete;
  VideoRingBuffer& operator=(const VideoRingBuffer&) = delete;

  // Open an existing shared memory file written by the native agent.
  bool Open(const std::string& path);

  // Close and unmap.
  void Close();

  // Read the latest frame. Returns pointer to NV12 frame data in
  // mapped memory (valid until next Read or Close), or nullptr if
  // no new frame since last read. Sets *out_seq to the sequence number.
  const uint8_t* ReadLatestFrame(uint32_t* out_seq);

  bool is_open() const { return mapped_ != nullptr; }
  int width() const;
  int height() const;
  int fps() const { return 30; }  // Native agent renders at 30fps
  size_t frame_size() const;

 private:
  static constexpr size_t kHeaderSize = sizeof(Header);

  std::string path_;
  int fd_ = -1;
  // RAW_PTR_EXCLUSION: pointers into mmap'd shared memory.
  RAW_PTR_EXCLUSION void* mapped_ = nullptr;
  size_t mapped_size_ = 0;

  RAW_PTR_EXCLUSION Header* header_ = nullptr;
  RAW_PTR_EXCLUSION uint8_t* frame_data_ = nullptr;

  uint32_t last_read_seq_ = 0;
};

}  // namespace asmodeus

#endif  // MEDIA_CAPTURE_VIDEO_ASMODEUS_VIDEO_RING_BUFFER_H_
