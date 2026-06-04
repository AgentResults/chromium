// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/handles/browser/frames_handle.h"

#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"

namespace aurelian {

std::vector<FrameInfo> ListFrames(content::WebContents* wc) {
  std::vector<FrameInfo> result;
  if (!wc || !wc->GetPrimaryMainFrame()) {
    return result;
  }
  wc->GetPrimaryMainFrame()->ForEachRenderFrameHost(
      [&result](content::RenderFrameHost* rfh) {
        result.push_back(
            {rfh->GetLastCommittedURL().spec(), rfh->IsInPrimaryMainFrame()});
      });
  return result;
}

}  // namespace aurelian
