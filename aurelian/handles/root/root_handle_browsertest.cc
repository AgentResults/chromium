// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian browser tests — the navigable legion://chrome/ root reaches a real
// browser capability by walking down the tree (root -> system -> info).

#include "aurelian/handles/root/root_handle.h"

#include "base/process/process.h"
#include "base/strings/string_number_conversions.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/test/browser_test.h"
#include "url/gurl.h"

namespace aurelian {

class AurelianRootBrowserTest : public InProcessBrowserTest {};

IN_PROC_BROWSER_TEST_F(AurelianRootBrowserTest, NavigatesToRealCapability) {
  ChromeRoot* root = CreateChromeRoot();
  ASSERT_NE(root, nullptr);

  // The root answers its identity.
  EXPECT_EQ(RootDispatch(root, "__getIdentity"), "legion://chrome/");

  // Walk root -> system -> info, reaching the REAL system capability: the
  // reported browser pid is this process.
  std::string info = RootDispatch(root, "system/info");
  EXPECT_NE(info.find("\"browserPid\":"), std::string::npos) << info;
  EXPECT_NE(info.find(std::to_string(base::Process::Current().Pid())),
            std::string::npos)
      << info;

  // An unknown child is broken.
  EXPECT_NE(RootDispatch(root, "bogus").find("broken"), std::string::npos);

  DestroyChromeRoot(root);
}

IN_PROC_BROWSER_TEST_F(AurelianRootBrowserTest, NavigatesToTabsCapability) {
  // The test browser has a real, live tab; point it at a known URL.
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>root-tabs</title>")));

  ChromeRoot* root = CreateChromeRoot();
  ASSERT_NE(root, nullptr);

  // Walk root -> tabs -> count, reaching the REAL live tab strip: at least the
  // one tab this browser test opened.
  std::string count = RootDispatch(root, "tabs/count");
  int n = 0;
  ASSERT_TRUE(base::StringToInt(count, &n)) << "count not an int: " << count;
  EXPECT_GE(n, 1) << count;

  // The active tab's URL is reachable too and reflects where we navigated.
  std::string active = RootDispatch(root, "tabs/activeUrl");
  EXPECT_NE(active.find("data:text/html"), std::string::npos) << active;

  DestroyChromeRoot(root);
}

}  // namespace aurelian
