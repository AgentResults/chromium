// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C15.a: system info — browser process + live renderer-process count
// (/system/info + /system/processes seam).

#ifndef AURELIAN_HANDLES_BROWSER_SYSTEM_HANDLE_H_
#define AURELIAN_HANDLES_BROWSER_SYSTEM_HANDLE_H_

#include <cstdint>

namespace aurelian {

struct SystemInfo {
  int64_t browser_pid = 0;
  // Number of live renderer processes (those backed by a valid OS process).
  int render_process_count = 0;
};

// Reads system-wide process info. UI thread.
SystemInfo GetSystemInfo();

}  // namespace aurelian

#endif  // AURELIAN_HANDLES_BROWSER_SYSTEM_HANDLE_H_
