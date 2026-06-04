// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C15.b: GPU info — the active GPU + GL strings the browser collected
// (/system gpu seam). A global query (no Browser*/Profile), like system_handle.

#ifndef AURELIAN_HANDLES_BROWSER_GPU_HANDLE_H_
#define AURELIAN_HANDLES_BROWSER_GPU_HANDLE_H_

#include <cstdint>
#include <string>

namespace aurelian {

struct GpuSummary {
  // The currently active GPU device.
  uint32_t vendor_id = 0;
  uint32_t device_id = 0;
  std::string driver_version;
  // GL strings (populated once the GPU process reports back; may be empty
  // early or under software rendering — still a real, reachable value).
  std::string gl_vendor;
  std::string gl_renderer;
};

// Reads the browser's collected GPU info. UI thread.
GpuSummary GetGpuSummary();

}  // namespace aurelian

#endif  // AURELIAN_HANDLES_BROWSER_GPU_HANDLE_H_
