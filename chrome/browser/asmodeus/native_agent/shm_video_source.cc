// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/asmodeus/native_agent/shm_video_source.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "base/logging.h"
#include "rtc_base/time_utils.h"
#include "third_party/libyuv/include/libyuv.h"

#include "chrome/browser/asmodeus/native_agent/video_shm.h"

namespace asmodeus {

ShmVideoSource::ShmVideoSource(const std::string& video_shm_path,
                                int width, int height, int fps)
    : webrtc::VideoTrackSource(/*remote=*/false),
      shm_path_(video_shm_path),
      width_(width), height_(height), fps_(fps) {}

ShmVideoSource::~ShmVideoSource() {
  Stop();
  if (mapped_) {
    munmap(mapped_, mapped_size_);
    close(fd_);
  }
}

webrtc::VideoSourceInterface<webrtc::VideoFrame>* ShmVideoSource::source() {
  return &broadcaster_;
}

void ShmVideoSource::Start() {
  // Open and mmap the video shm
  fd_ = open(shm_path_.c_str(), O_RDONLY);
  if (fd_ < 0) {
    LOG(ERROR) << "ShmVideoSource: Failed to open: " << shm_path_;
    return;
  }

  uint32_t frame_size = width_ * height_ * 3 / 2;
  mapped_size_ = sizeof(VideoShmHeader) + frame_size * 2;
  mapped_ = mmap(nullptr, mapped_size_, PROT_READ, MAP_SHARED, fd_, 0);
  if (mapped_ == MAP_FAILED) {
    LOG(ERROR) << "ShmVideoSource: mmap failed";
    close(fd_);
    fd_ = -1;
    mapped_ = nullptr;
    return;
  }

  running_ = true;
  capture_thread_ = std::make_unique<std::thread>(
      &ShmVideoSource::CaptureLoop, this);
}

void ShmVideoSource::Stop() {
  running_ = false;
  if (capture_thread_ && capture_thread_->joinable()) {
    capture_thread_->join();
  }
  capture_thread_.reset();
}

void ShmVideoSource::CaptureLoop() {
  const int interval_ms = 1000 / fps_;

  while (running_) {
    if (!mapped_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
      continue;
    }

    auto* header = reinterpret_cast<const VideoShmHeader*>(mapped_);
    uint32_t seq = header->frame_sequence.load();

    // Only send if frame changed
    if (seq != last_sequence_) {
      last_sequence_ = seq;

      uint32_t current = header->current_buffer.load();
      uint32_t frame_size = header->frame_size;
      const uint8_t* base =
          reinterpret_cast<const uint8_t*>(mapped_) + sizeof(VideoShmHeader);
      const uint8_t* nv12 = base + current * frame_size;
      const uint8_t* y_plane = nv12;
      const uint8_t* uv_plane = nv12 + width_ * height_;

      // Convert NV12 → I420 for WebRTC
      auto i420_buffer = webrtc::I420Buffer::Create(width_, height_);
      libyuv::NV12ToI420(y_plane, width_,
                          uv_plane, width_,
                          i420_buffer->MutableDataY(), i420_buffer->StrideY(),
                          i420_buffer->MutableDataU(), i420_buffer->StrideU(),
                          i420_buffer->MutableDataV(), i420_buffer->StrideV(),
                          width_, height_);

      webrtc::VideoFrame frame =
          webrtc::VideoFrame::Builder()
              .set_video_frame_buffer(i420_buffer)
              .set_timestamp_us(webrtc::TimeMicros())
              .build();

      broadcaster_.OnFrame(frame);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
  }
}

}  // namespace asmodeus
