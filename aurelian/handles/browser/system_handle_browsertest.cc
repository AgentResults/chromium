// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C15.a browser tests — system info (browser pid + renderer count).

#include "aurelian/handles/browser/system_handle.h"

#include "base/process/process.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/test/browser_test.h"
#include "url/gurl.h"

namespace aurelian {

using AurelianSystemBrowserTest = InProcessBrowserTest;

IN_PROC_BROWSER_TEST_F(AurelianSystemBrowserTest, ReportsBrowserAndRenderers) {
  // Ensure at least one live renderer exists.
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>sys</title>sys")));

  SystemInfo info = GetSystemInfo();

  // The browser pid is this process.
  EXPECT_GT(info.browser_pid, 0);
  EXPECT_EQ(info.browser_pid, base::Process::Current().Pid());

  // At least one live renderer process is counted.
  EXPECT_GE(info.render_process_count, 1);
}

}  // namespace aurelian
