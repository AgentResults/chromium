// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C7.d browser tests — CDP-compat command surface.

#include "aurelian/handles/browser/devtools_handle.h"

#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace aurelian {

class AurelianDevtoolsBrowserTest : public InProcessBrowserTest {
 protected:
  content::WebContents* GetWC() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }
};

IN_PROC_BROWSER_TEST_F(AurelianDevtoolsBrowserTest, CdpCompatCommand) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>cdp</title>cdp body")));

  // A representative CDP command: Runtime.evaluate round-trips to the renderer.
  std::string resp = SendCdpCommand(
      GetWC(), "Runtime.evaluate",
      "{\"expression\":\"40+2\",\"returnByValue\":true}");

  EXPECT_NE(resp.find("\"id\":1"), std::string::npos) << resp;
  EXPECT_NE(resp.find("\"result\""), std::string::npos) << resp;
  EXPECT_NE(resp.find("\"value\":42"), std::string::npos)
      << "expected Runtime.evaluate to return 42; got: " << resp;
}

}  // namespace aurelian
