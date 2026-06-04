// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C11.a browser tests — emulation: user-agent override applies to the
// page.

#include "aurelian/handles/browser/emulation_handle.h"

#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "url/gurl.h"

namespace aurelian {

class AurelianEmulationBrowserTest : public InProcessBrowserTest {
 protected:
  content::WebContents* GetWC() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }
};

IN_PROC_BROWSER_TEST_F(AurelianEmulationBrowserTest, EmulatesUserAgent) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>ua</title>")));
  content::WebContents* wc = GetWC();

  // No override initially.
  EXPECT_EQ(GetUserAgentOverride(wc), "");

  // Set + read back the override.
  SetUserAgentOverride(wc, "AurelianBot/1.0");
  EXPECT_EQ(GetUserAgentOverride(wc), "AurelianBot/1.0");

  // Navigate WITH the override active; the page must observe it.
  content::NavigationController::LoadURLParams params(
      GURL("data:text/html,<title>ua2</title>ua body"));
  params.override_user_agent =
      content::NavigationController::UA_OVERRIDE_TRUE;
  wc->GetController().LoadURLWithParams(params);
  ASSERT_TRUE(content::WaitForLoadStop(wc));

  EXPECT_EQ(content::EvalJs(wc, "navigator.userAgent").ExtractString(),
            "AurelianBot/1.0");
}

}  // namespace aurelian
