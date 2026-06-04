// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/handles/browser/process_handle.h"

#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/visibility.h"
#include "content/public/browser/web_contents.h"

namespace aurelian {

TabProcessInfo GetTabProcessInfo(content::WebContents* wc) {
  TabProcessInfo info;
  if (!wc) {
    return info;
  }
  content::RenderProcessHost* rph = wc->GetPrimaryMainFrame()->GetProcess();
  if (rph && rph->GetProcess().IsValid()) {
    info.renderer_pid = rph->GetProcess().Pid();
  }
  info.visible = wc->GetVisibility() == content::Visibility::VISIBLE;
  return info;
}

}  // namespace aurelian
