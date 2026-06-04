// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C12.a: per-tab process info — the OS process backing a tab
// (/system/processes + /browser/tabs/<id>/memory seam).

#ifndef AURELIAN_HANDLES_BROWSER_PROCESS_HANDLE_H_
#define AURELIAN_HANDLES_BROWSER_PROCESS_HANDLE_H_

#include <cstdint>

namespace content {
class WebContents;
}

namespace aurelian {

struct TabProcessInfo {
  // OS process id of the tab's primary-main-frame renderer (0 if none).
  int64_t renderer_pid = 0;
  bool visible = false;
};

// Reads the process info for `wc`'s primary main frame. UI thread.
TabProcessInfo GetTabProcessInfo(content::WebContents* wc);

}  // namespace aurelian

#endif  // AURELIAN_HANDLES_BROWSER_PROCESS_HANDLE_H_
