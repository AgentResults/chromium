// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian AU-TAB-REACH: the INTERACTIVE tab verbs (open / activate / close +
// per-tab url) are reachable from the sealed root via RootDispatch — the SAME
// entry a remote controller walks — and they MUTATE the live tab strip. Before
// this mount the per-tab/interactive paths returned broken('unknown-message'):
// the tabstrip handle was built (tabstrip_handle.cc, with its own browsertest)
// but mounted on nothing, so no controller could reach it. This test drives the
// verbs end-to-end against a real multi-tab browser and asserts the live strip
// actually changes.

#include "aurelian/handles/root/root_handle.h"

#include "chrome/browser/ui/browser.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/test/browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace aurelian {

using AurelianTabReachBrowserTest = InProcessBrowserTest;

IN_PROC_BROWSER_TEST_F(AurelianTabReachBrowserTest, ControllerDrivesLiveTabStrip) {
  ChromeRoot* root = CreateChromeRoot();

  // The browser starts with exactly one tab.
  EXPECT_EQ(RootDispatch(root, "tabs/count"), "1");

  // Open a second tab THROUGH the mounted interactive verb. OpenTab foregrounds
  // it, so the new tab (index 1) becomes active and its index is returned.
  EXPECT_EQ(RootDispatch(root, "tabs/open"), "1")
      << "tabs/open is unreachable — the interactive tab handle is not mounted";
  EXPECT_EQ(RootDispatch(root, "tabs/count"), "2");
  EXPECT_EQ(RootDispatch(root, "tabs/activeIndex"), "1");

  // `activate` genuinely moves the active tab on the LIVE strip — both ways.
  EXPECT_EQ(RootDispatch(root, "tabs/0/activate"), "ok");
  EXPECT_EQ(RootDispatch(root, "tabs/activeIndex"), "0");
  EXPECT_EQ(RootDispatch(root, "tabs/1/activate"), "ok");
  EXPECT_EQ(RootDispatch(root, "tabs/activeIndex"), "1");

  // The per-tab handle reads real committed state: navigate the active tab
  // (tab 1) to a data: URL and read it back through `tabs/1/url`.
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>reach</title>tab-reach")));
  EXPECT_EQ(RootDispatch(root, "tabs/1/url"),
            "data:text/html,<title>reach</title>tab-reach");

  // `close` shrinks the live strip.
  EXPECT_EQ(RootDispatch(root, "tabs/1/close"), "ok");
  EXPECT_EQ(RootDispatch(root, "tabs/count"), "1");

  DestroyChromeRoot(root);
}

}  // namespace aurelian
