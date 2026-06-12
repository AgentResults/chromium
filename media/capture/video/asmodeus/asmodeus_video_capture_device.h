// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_CAPTURE_VIDEO_ASMODEUS_ASMODEUS_VIDEO_CAPTURE_DEVICE_H_
#define MEDIA_CAPTURE_VIDEO_ASMODEUS_ASMODEUS_VIDEO_CAPTURE_DEVICE_H_

#include <memory>
#include <string>

#include "base/timer/timer.h"
#include "media/capture/video/asmodeus/video_ring_buffer.h"
#include "media/capture/video/video_capture_device.h"

namespace asmodeus {

// Virtual camera device that reads NV12 frames from a shared memory
// buffer written by the native agent's AvatarRenderer.
//
// The native agent renders an animated face with lip sync and writes
// NV12 frames to video-in-{name}.shm. This device reads those frames
// and delivers them to Chrome's video capture pipeline, making the
// avatar appear as a real webcam in Google Meet.
class AsmodeusVideoCaptureDevice : public media::VideoCaptureDevice {
 public:
  static constexpr char kDeviceIdPrefix[] = "asmodeus-cam-";
  static constexpr char kDeviceNamePrefix[] = "Asmodeus Camera: ";

  AsmodeusVideoCaptureDevice(const std::string& name,
                             const std::string& shm_path);
  ~AsmodeusVideoCaptureDevice() override;

  AsmodeusVideoCaptureDevice(const AsmodeusVideoCaptureDevice&) = delete;
  AsmodeusVideoCaptureDevice& operator=(const AsmodeusVideoCaptureDevice&) =
      delete;

  // VideoCaptureDevice implementation.
  void AllocateAndStart(
      const media::VideoCaptureParams& params,
      std::unique_ptr<media::VideoCaptureDevice::Client> client) override;
  void StopAndDeAllocate() override;

 private:
  void OnTimer();

  std::string name_;
  std::string shm_path_;
  std::unique_ptr<VideoRingBuffer> ring_buffer_;
  std::unique_ptr<media::VideoCaptureDevice::Client> client_;
  base::RepeatingTimer timer_;
  media::VideoCaptureFormat format_;
  base::TimeTicks first_frame_time_;
  uint32_t frames_delivered_ = 0;
  bool has_first_frame_ = false;
  std::vector<uint8_t> i420_buffer_;
  size_t i420_buffer_size_ = 0;
};

}  // namespace asmodeus

#endif  // MEDIA_CAPTURE_VIDEO_ASMODEUS_ASMODEUS_VIDEO_CAPTURE_DEVICE_H_
