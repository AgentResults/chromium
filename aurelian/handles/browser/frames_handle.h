// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C2.x: frame tree — list all frames in a tab (/frames; C3.6 shipped a
// single-frame dispatch handle, not the tree listing).

#ifndef AURELIAN_HANDLES_BROWSER_FRAMES_HANDLE_H_
#define AURELIAN_HANDLES_BROWSER_FRAMES_HANDLE_H_

#include <string>
#include <vector>

namespace content {
class WebContents;
}

namespace aurelian {

struct FrameInfo {
  std::string url;
  bool is_main = false;
};

// Lists every frame in `wc` (the primary main frame + its descendants),
// including the main frame flag. UI thread.
std::vector<FrameInfo> ListFrames(content::WebContents* wc);

}  // namespace aurelian

#endif  // AURELIAN_HANDLES_BROWSER_FRAMES_HANDLE_H_
