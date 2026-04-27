// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ASMODEUS_NATIVE_AGENT_SHM_VIDEO_SOURCE_H_
#define CHROME_BROWSER_ASMODEUS_NATIVE_AGENT_SHM_VIDEO_SOURCE_H_

#include <atomic>
#include <string>
#include <thread>

#include "api/scoped_refptr.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "pc/video_track_source.h"
#include "media/base/video_broadcaster.h"

namespace asmodeus {

// Video source that reads NV12 frames from shared memory and
// broadcasts them as I420 to WebRTC video tracks.
class ShmVideoSource : public webrtc::VideoTrackSource {
 public:
  ShmVideoSource(const std::string& video_shm_path,
                  int width, int height, int fps);
  ~ShmVideoSource() override;

  void Start();
  void Stop();

 protected:
  webrtc::VideoSourceInterface<webrtc::VideoFrame>* source() override;

 private:
  void CaptureLoop();

  webrtc::VideoBroadcaster broadcaster_;
  std::string shm_path_;
  int width_, height_, fps_;
  std::atomic<bool> running_{false};
  std::unique_ptr<std::thread> capture_thread_;

  // Video shm mmap
  RAW_PTR_EXCLUSION void* mapped_ = nullptr;
  size_t mapped_size_ = 0;
  int fd_ = -1;
  uint32_t last_sequence_ = 0;
};

}  // namespace asmodeus

#endif  // CHROME_BROWSER_ASMODEUS_NATIVE_AGENT_SHM_VIDEO_SOURCE_H_
