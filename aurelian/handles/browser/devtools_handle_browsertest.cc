// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C7.d browser tests — CDP-compat command surface.

#include "aurelian/handles/browser/devtools_handle.h"

#include "aurelian/handles/root/root_handle.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/test_navigation_observer.h"
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

IN_PROC_BROWSER_TEST_F(AurelianDevtoolsBrowserTest, CapturesCdpEvent) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>evt</title>")));

  // Enable Runtime, evaluate a console.log, capture the consoleAPICalled event.
  std::string evt = CaptureCdpEvent(
      GetWC(), "Runtime.enable", "Runtime.evaluate",
      "{\"expression\":\"console.log('cdp-evt-42')\"}",
      "Runtime.consoleAPICalled");

  EXPECT_NE(evt.find("consoleAPICalled"), std::string::npos) << evt;
  EXPECT_NE(evt.find("cdp-evt-42"), std::string::npos) << evt;
}

// ACM-S1 (precondition pin) — a navigation driven ENTIRELY through the
// existing CDP path (Page.navigate on the per-WebContents devtools
// session) is observed by the C1 facade: legion://chrome/tabs/activeUrl
// reports the navigated URL. This pins the baseline the HS-1 async
// session layer must preserve when it replaces this path
// (AURELIAN-GENERIC-CONTROL-TDD-PLAN Phase 0, ACM-S1).
IN_PROC_BROWSER_TEST_F(AurelianDevtoolsBrowserTest,
                       CdpNavigateObservedByFacade) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>before</title>start")));

  content::TestNavigationObserver nav_observer(GetWC());
  std::string resp = SendCdpCommand(
      GetWC(), "Page.navigate",
      "{\"url\":\"data:text/html,<title>after</title>acm-s1-navigated\"}");
  EXPECT_NE(resp.find("\"id\":1"), std::string::npos) << resp;
  nav_observer.Wait();

  ChromeRoot* root = CreateChromeRoot();
  ASSERT_NE(root, nullptr);
  std::string active = RootDispatch(root, "tabs/activeUrl").reply;
  EXPECT_NE(active.find("acm-s1-navigated"), std::string::npos)
      << "facade does not observe the CDP-driven navigation: " << active;
  DestroyChromeRoot(root);
}

}  // namespace aurelian
