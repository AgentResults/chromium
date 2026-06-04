// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian browser tests — the navigable legion://chrome/ root reaches a real
// browser capability by walking down the tree (root -> system -> info).

#include "aurelian/handles/root/root_handle.h"

#include "base/process/process.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "content/public/test/browser_test.h"

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

}  // namespace aurelian
