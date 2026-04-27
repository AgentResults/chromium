// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ASMODEUS_NATIVE_AGENT_VIDEO_SHM_H_
#define CHROME_BROWSER_ASMODEUS_NATIVE_AGENT_VIDEO_SHM_H_

#include <atomic>
#include <cstdint>

namespace asmodeus {

// Video shared memory header layout.
// Followed by two NV12 frame buffers (double-buffered).
struct VideoShmHeader {
  uint32_t width;
  uint32_t height;
  uint32_t frame_size;  // width * height * 3 / 2 for NV12
  std::atomic<uint32_t> current_buffer;  // 0 or 1
  std::atomic<uint32_t> frame_sequence;  // incremented on each new frame
};

}  // namespace asmodeus

#endif  // CHROME_BROWSER_ASMODEUS_NATIVE_AGENT_VIDEO_SHM_H_
