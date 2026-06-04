// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C1.x browser tests — tabstrip open / close / activate.

#include "aurelian/handles/browser/tabstrip_handle.h"

#include "chrome/browser/ui/browser.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "content/public/test/browser_test.h"

namespace aurelian {

using AurelianTabStripBrowserTest = InProcessBrowserTest;

IN_PROC_BROWSER_TEST_F(AurelianTabStripBrowserTest, OpenActivateClose) {
  Browser* b = browser();
  EXPECT_EQ(TabCount(b), 1);

  // Open a second tab in the foreground.
  int idx = OpenTab(b, "about:blank", /*foreground=*/true);
  EXPECT_EQ(idx, 1);
  EXPECT_EQ(TabCount(b), 2);
  EXPECT_EQ(ActiveTabIndex(b), 1);

  // Activate the first tab.
  EXPECT_TRUE(ActivateTab(b, 0));
  EXPECT_EQ(ActiveTabIndex(b), 0);

  // Close the second tab.
  EXPECT_TRUE(CloseTab(b, 1));
  EXPECT_EQ(TabCount(b), 1);

  // Closing the last remaining tab is refused (out of scope — window close).
  EXPECT_FALSE(CloseTab(b, 0));
  EXPECT_EQ(TabCount(b), 1);

  // Bad index.
  EXPECT_FALSE(ActivateTab(b, 5));
}

}  // namespace aurelian
