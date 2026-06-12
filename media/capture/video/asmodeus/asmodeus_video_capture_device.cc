// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/capture/video/asmodeus/asmodeus_video_capture_device.h"

#include <algorithm>
#include <cstring>

#include "base/logging.h"
#include "base/time/time.h"
#include "media/base/video_frame.h"
#include "media/base/video_types.h"

namespace asmodeus {

AsmodeusVideoCaptureDevice::AsmodeusVideoCaptureDevice(
    const std::string& name,
    const std::string& shm_path)
    : name_(name), shm_path_(shm_path) {}

AsmodeusVideoCaptureDevice::~AsmodeusVideoCaptureDevice() {
  StopAndDeAllocate();
}

void AsmodeusVideoCaptureDevice::AllocateAndStart(
    const media::VideoCaptureParams& params,
    std::unique_ptr<media::VideoCaptureDevice::Client> client) {
  client_ = std::move(client);

  ring_buffer_ = std::make_unique<VideoRingBuffer>();
  if (!ring_buffer_->Open(shm_path_)) {
    LOG(ERROR) << "[Asmodeus] VideoCaptureDevice: failed to open " << shm_path_;
    client_->OnError(
        media::VideoCaptureError::kMacCouldNotStartCaptureDevice,
        FROM_HERE, "Failed to open Asmodeus video shm");
    return;
  }

  const int w = ring_buffer_->width();
  const int h = ring_buffer_->height();
  const int fps = ring_buffer_->fps();
  const size_t fsz = ring_buffer_->frame_size();

  // Native agent writes NV12 frames directly.
  format_ = media::VideoCaptureFormat(
      gfx::Size(w, h),
      static_cast<float>(fps),
      media::PIXEL_FORMAT_NV12);

  // Buffer to hold NV12 frame data for delivery.
  i420_buffer_size_ = fsz;
  i420_buffer_.resize(i420_buffer_size_);

  LOG(WARNING) << "[Asmodeus] VideoCaptureDevice '" << name_
               << "' started: " << w << "x" << h << "@" << fps
               << " NV12 frame_size=" << fsz
               << " shm=" << shm_path_;

  client_->OnStarted();

  const base::TimeDelta interval =
      base::Milliseconds(1000 / std::max(fps, 1));
  first_frame_time_ = base::TimeTicks::Now();
  frames_delivered_ = 0;

  timer_.Start(FROM_HERE, interval,
               base::BindRepeating(&AsmodeusVideoCaptureDevice::OnTimer,
                                   base::Unretained(this)));
}

void AsmodeusVideoCaptureDevice::StopAndDeAllocate() {
  timer_.Stop();
  if (ring_buffer_) {
    ring_buffer_->Close();
    ring_buffer_.reset();
  }
  client_.reset();
}

void AsmodeusVideoCaptureDevice::OnTimer() {
  if (!client_ || !ring_buffer_) return;

  const int w = ring_buffer_->width();
  const int h = ring_buffer_->height();
  const size_t fsz = ring_buffer_->frame_size();
  const auto now = base::TimeTicks::Now();
  const auto timestamp = now - first_frame_time_;

  uint32_t seq = 0;
  const uint8_t* frame = ring_buffer_->ReadLatestFrame(&seq);

  if (!frame) {
    // No new frame — re-deliver the previous frame.
    // If we have never received a frame, deliver black NV12.
    if (!has_first_frame_) {
      UNSAFE_BUFFERS([&] {
        // Y plane = black (16), UV plane = neutral (128)
        memset(i420_buffer_.data(), 16, static_cast<size_t>(w * h));
        uint8_t* uv = i420_buffer_.data() + w * h;
        const size_t uv_size = fsz - static_cast<size_t>(w * h);
        memset(uv, 128, uv_size);
      }());
    }
    // Otherwise i420_buffer_ still has the last frame — re-send it.
  } else {
    has_first_frame_ = true;
    // Data is already NV12 from native agent — just copy it.
    UNSAFE_BUFFERS(memcpy(i420_buffer_.data(), frame, fsz));
  }

  client_->OnIncomingCapturedData(
      i420_buffer_.data(), static_cast<int>(i420_buffer_size_),
      format_,
      gfx::ColorSpace::CreateREC601(),
      0 /* rotation */, false /* flip_y */,
      now, timestamp,
      /*capture_begin_timestamp=*/std::nullopt,
      /*metadata=*/std::nullopt,
      /*frame_feedback_id=*/0);

  ++frames_delivered_;

  if (frames_delivered_ <= 3 || frames_delivered_ % 300 == 0) {
    LOG(WARNING) << "[Asmodeus] VideoCaptureDevice '" << name_
                 << "' frame=" << frames_delivered_
                 << " seq=" << seq
                 << " has_data=" << (frame != nullptr);
  }
}

}  // namespace asmodeus
