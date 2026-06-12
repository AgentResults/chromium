// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/capture/video/asmodeus/video_ring_buffer.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "base/compiler_specific.h"
#include "base/logging.h"

namespace asmodeus {

VideoRingBuffer::VideoRingBuffer() = default;

VideoRingBuffer::~VideoRingBuffer() {
  Close();
}

bool VideoRingBuffer::Open(const std::string& path) {
  Close();
  path_ = path;

  fd_ = open(path.c_str(), O_RDWR);
  if (fd_ < 0) {
    LOG(ERROR) << "[Asmodeus] VideoRingBuffer::Open failed: " << path
               << " errno=" << errno;
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
    LOG(ERROR) << "[Asmodeus] VideoRingBuffer: file too small: " << mapped_size_;
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
  // SAFETY: frame_data_ points into mmap'd region after the header.
  frame_data_ = UNSAFE_BUFFERS(static_cast<uint8_t*>(mapped_) + kHeaderSize);

  LOG(WARNING) << "[Asmodeus] VideoRingBuffer::Open " << path
               << " w=" << width() << " h=" << height()
               << " frame_size=" << frame_size()
               << " seq=" << header_->frame_sequence.load()
               << " file_size=" << mapped_size_;
  return true;
}

void VideoRingBuffer::Close() {
  if (mapped_) {
    munmap(mapped_, mapped_size_);
    mapped_ = nullptr;
  }
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }
  header_ = nullptr;
  frame_data_ = nullptr;
}

const uint8_t* VideoRingBuffer::ReadLatestFrame(uint32_t* out_seq) {
  if (!header_ || !frame_data_) return nullptr;

  const uint32_t seq = header_->frame_sequence.load(std::memory_order_acquire);
  if (seq <= last_read_seq_) return nullptr;  // No new frame.

  const uint32_t buf_idx = header_->current_buffer.load(
      std::memory_order_acquire);
  const uint32_t fsz = header_->frame_size;

  last_read_seq_ = seq;
  if (out_seq) *out_seq = seq;

  // Double-buffered: read from buffer[current_buffer]
  // SAFETY: buf_idx is 0 or 1, frame_data_ has 2 * fsz bytes.
  return UNSAFE_BUFFERS(frame_data_ + buf_idx * fsz);
}

int VideoRingBuffer::width() const {
  return header_ ? static_cast<int>(header_->width) : 0;
}

int VideoRingBuffer::height() const {
  return header_ ? static_cast<int>(header_->height) : 0;
}

size_t VideoRingBuffer::frame_size() const {
  return header_ ? header_->frame_size : 0;
}

}  // namespace asmodeus
