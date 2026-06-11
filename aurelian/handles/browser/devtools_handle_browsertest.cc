// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C7.d browser tests — CDP-compat command surface. The two
// page-session pins below migrated off SendCdpCommand onto the ACM-3
// per-target sub-mirror (TEST-CHANGE, plan ACM-3 / design section 7: the
// one-shot machinery is deleted when its last caller migrates — the
// remaining CapturesCdpEvent migrates at ACM-4). The assertions are
// unchanged in substance: the same commands, the same observable results,
// now through the replacement surface they pinned the baseline for.

#include "aurelian/handles/browser/devtools_handle.h"

#include "aurelian/handles/root/root_handle.h"
#include "aurelian/handles/root/wire_serialize.h"
#include "base/test/run_until.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/test_navigation_observer.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"
#include "velite/agentspaces-wire/handle.hpp"

namespace aurelian {

namespace {

[[nodiscard]] bool WaitUntilSettled(
    const std::shared_ptr<velite::agentspaces::Handle>& answer) {
  return base::test::RunUntil([&]() {
    return answer->state_kind() != velite::agentspaces::StateKind::Pending;
  });
}

}  // namespace

class AurelianDevtoolsBrowserTest : public InProcessBrowserTest {
 protected:
  content::WebContents* GetWC() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }

  std::string ActivePageTargetId() {
    return content::DevToolsAgentHost::GetOrCreateFor(GetWC())->GetId();
  }
};

IN_PROC_BROWSER_TEST_F(AurelianDevtoolsBrowserTest, CdpCompatCommand) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>cdp</title>cdp body")));

  // A representative CDP command: Runtime.evaluate round-trips to the
  // renderer — through the per-target sub-mirror (ACM-3).
  ChromeRoot* root = CreateChromeRoot();
  ASSERT_NE(root, nullptr);
  DispatchOutcome outcome = RootDispatch(
      root, "targets/" + ActivePageTargetId() + "/cdp/Runtime/evaluate/invoke",
      "{\"expression\":\"40+2\",\"returnByValue\":true}");
  ASSERT_EQ(outcome.kind, DispatchOutcome::Kind::kPending) << outcome.reply;
  ASSERT_TRUE(WaitUntilSettled(outcome.answer));
  ASSERT_EQ(outcome.answer->state_kind(),
            velite::agentspaces::StateKind::ResolvedValue)
      << outcome.answer->broken_reason();

  std::string resp = SerializeWireReply(outcome.answer);
  EXPECT_NE(resp.find("\"result\""), std::string::npos) << resp;
  EXPECT_NE(resp.find("\"value\":42"), std::string::npos)
      << "expected Runtime.evaluate to return 42; got: " << resp;
  DestroyChromeRoot(root);
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
// CDP path is observed by the C1 facade: legion://chrome/tabs/activeUrl
// reports the navigated URL. Pinned on the one-shot path at ACM-S1; the
// HS-1 replacement (the per-target sub-mirror, ACM-3) must preserve it —
// the pin migrated onto the replacement here (TEST-CHANGE).
IN_PROC_BROWSER_TEST_F(AurelianDevtoolsBrowserTest,
                       CdpNavigateObservedByFacade) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>before</title>start")));

  ChromeRoot* root = CreateChromeRoot();
  ASSERT_NE(root, nullptr);

  content::TestNavigationObserver nav_observer(GetWC());
  DispatchOutcome outcome = RootDispatch(
      root, "targets/" + ActivePageTargetId() + "/cdp/Page/navigate/invoke",
      "{\"url\":\"data:text/html,<title>after</title>acm-s1-navigated\"}");
  ASSERT_EQ(outcome.kind, DispatchOutcome::Kind::kPending) << outcome.reply;
  ASSERT_TRUE(WaitUntilSettled(outcome.answer));
  ASSERT_EQ(outcome.answer->state_kind(),
            velite::agentspaces::StateKind::ResolvedValue)
      << outcome.answer->broken_reason();
  nav_observer.Wait();

  std::string active = RootDispatch(root, "tabs/activeUrl").reply;
  EXPECT_NE(active.find("acm-s1-navigated"), std::string::npos)
      << "facade does not observe the CDP-driven navigation: " << active;
  DestroyChromeRoot(root);
}

}  // namespace aurelian
