// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C12.a browser tests — per-tab process info.

#include "aurelian/handles/browser/process_handle.h"

#include "base/process/process.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "url/gurl.h"

namespace aurelian {

class AurelianProcessBrowserTest : public InProcessBrowserTest {
 protected:
  content::WebContents* GetWC() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }
};

IN_PROC_BROWSER_TEST_F(AurelianProcessBrowserTest, ReportsRendererProcess) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>proc</title>proc")));
  content::WebContents* wc = GetWC();

  TabProcessInfo info = GetTabProcessInfo(wc);

  // A live renderer process backs the tab.
  EXPECT_GT(info.renderer_pid, 0);

  // It is the RENDERER process, distinct from this (the browser) process.
  EXPECT_NE(info.renderer_pid, base::Process::Current().Pid());

  // And it matches the main frame's actual process id.
  EXPECT_EQ(info.renderer_pid,
            wc->GetPrimaryMainFrame()->GetProcess()->GetProcess().Pid());
}

}  // namespace aurelian
