// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C1.y browser tests — windows list.

#include "aurelian/handles/browser/windows_handle.h"

#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "content/public/test/browser_test.h"

namespace aurelian {

using AurelianWindowsBrowserTest = InProcessBrowserTest;

IN_PROC_BROWSER_TEST_F(AurelianWindowsBrowserTest, ListsWindows) {
  // One window initially, with at least one tab and a non-zero size.
  std::vector<WindowInfo> windows = ListWindows(browser()->profile());
  ASSERT_EQ(windows.size(), 1u);
  EXPECT_GE(windows[0].tab_count, 1);
  EXPECT_GT(windows[0].width, 0);
  EXPECT_GT(windows[0].height, 0);

  // Opening a second browser window is reflected.
  CreateBrowser(browser()->profile());
  windows = ListWindows(browser()->profile());
  EXPECT_EQ(windows.size(), 2u);
}

}  // namespace aurelian
