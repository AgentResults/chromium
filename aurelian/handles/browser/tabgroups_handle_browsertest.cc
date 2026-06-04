// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C13.c browser tests — tab groups group / query / ungroup.

#include "aurelian/handles/browser/tabgroups_handle.h"

#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_tabstrip.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "content/public/test/browser_test.h"
#include "url/gurl.h"

namespace aurelian {

class AurelianTabGroupsBrowserTest : public InProcessBrowserTest {
 protected:
  TabStripModel* Model() { return browser()->tab_strip_model(); }
};

IN_PROC_BROWSER_TEST_F(AurelianTabGroupsBrowserTest, GroupQueryUngroup) {
  // Need at least two tabs.
  chrome::AddTabAt(browser(), GURL("about:blank"), -1, /*foreground=*/true);
  ASSERT_GE(Model()->count(), 2);
  ASSERT_TRUE(Model()->SupportsTabGroups());

  // Initially ungrouped.
  EXPECT_EQ(GroupOfTab(Model(), 0), "");

  // Group tabs 0 and 1.
  std::string gid = GroupTabs(Model(), {0, 1});
  ASSERT_FALSE(gid.empty());
  EXPECT_EQ(GroupOfTab(Model(), 0), gid);
  EXPECT_EQ(GroupOfTab(Model(), 1), gid);

  // Ungroup tab 0; tab 1 stays grouped.
  EXPECT_TRUE(UngroupTab(Model(), 0));
  EXPECT_EQ(GroupOfTab(Model(), 0), "");
  EXPECT_EQ(GroupOfTab(Model(), 1), gid);

  // Ungrouping an already-ungrouped tab fails.
  EXPECT_FALSE(UngroupTab(Model(), 0));
}

IN_PROC_BROWSER_TEST_F(AurelianTabGroupsBrowserTest, ListsGroups) {
  chrome::AddTabAt(browser(), GURL("about:blank"), -1, /*foreground=*/true);
  ASSERT_GE(Model()->count(), 2);

  // No groups yet.
  EXPECT_TRUE(ListGroups(Model()).empty());

  std::string gid = GroupTabs(Model(), {0, 1});
  ASSERT_FALSE(gid.empty());

  std::vector<std::string> groups = ListGroups(Model());
  ASSERT_EQ(groups.size(), 1u);
  EXPECT_EQ(groups[0], gid);
}

}  // namespace aurelian
