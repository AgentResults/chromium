// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/handles/browser/system_handle.h"

#include "base/process/process.h"
#include "content/public/browser/render_process_host.h"

namespace aurelian {

SystemInfo GetSystemInfo() {
  SystemInfo info;
  info.browser_pid = base::Process::Current().Pid();
  int count = 0;
  for (content::RenderProcessHost::iterator it =
           content::RenderProcessHost::AllHostsIterator();
       !it.IsAtEnd(); it.Advance()) {
    content::RenderProcessHost* rph = it.GetCurrentValue();
    if (rph && rph->GetProcess().IsValid()) {
      ++count;
    }
  }
  info.render_process_count = count;
  return info;
}

}  // namespace aurelian
