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
// (ACM-R F6c: the leftover devtools_handle.h include went — no symbol
// from it is used here since the ACM-3/4 migrations.)

#include <memory>
#include <string>
#include <vector>

#include "aurelian/handles/root/root_handle.h"
#include "aurelian/handles/root/wire_serialize.h"
#include "aurelian/mirror/cdp_mirror.h"
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
#include "velite/agentspaces-wire/value_handle.hpp"

namespace aurelian {

namespace {

[[nodiscard]] bool WaitUntilSettled(
    const std::shared_ptr<velite::agentspaces::Handle>& answer) {
  return base::test::RunUntil([&]() {
    return answer->state_kind() != velite::agentspaces::StateKind::Pending;
  });
}

// The substrate sink (ACM-4): records every `legion-notify` frame.
class RecordingSink : public velite::agentspaces::Handle {
 public:
  velite::agentspaces::StateKind state_kind() const override {
    return velite::agentspaces::StateKind::ResolvedValue;
  }
  const velite::agentspaces::Value& resolved_value() const override {
    return identity_;
  }
  std::shared_ptr<velite::agentspaces::Handle> resolved_handle()
      const override {
    return nullptr;
  }
  std::string_view broken_reason() const override { return ""; }
  std::string sturdy_identity() const override { return "test://sink"; }

  std::shared_ptr<velite::agentspaces::Handle> ask_impl(
      std::string_view,
      const velite::agentspaces::Value&) override {
    return velite::agentspaces::ValueHandle::make_broken("unknown-message");
  }

  void tell(std::string_view msg,
            const velite::agentspaces::Value& frame) override {
    if (msg == "legion-notify") {
      frames_.push_back(frame);
    }
  }

  const std::vector<velite::agentspaces::Value>& frames() const {
    return frames_;
  }

 private:
  std::vector<velite::agentspaces::Value> frames_;
  velite::agentspaces::Value identity_{std::string("test://sink")};
};

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

// (ACM-4 TEST-CHANGE) The event pin migrates off the CaptureCdpEvent
// one-shot — deleted in the same commit, its last caller — onto the
// subscribe surface: same observable (a Runtime.consoleAPICalled event
// carrying the logged text), now via subscribe({sink}) on the per-target
// sub-mirror with the domain auto-enabled and the action invoked through
// the same session layer.
IN_PROC_BROWSER_TEST_F(AurelianDevtoolsBrowserTest, CapturesCdpEvent) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>evt</title>")));
  std::string id = ActivePageTargetId();

  auto sink = std::make_shared<RecordingSink>();
  std::shared_ptr<velite::agentspaces::Handle> event_node =
      CreateCdpMirrorForTarget(id, "page")
          ->ask("Runtime", velite::agentspaces::Value())
          ->ask("consoleAPICalled", velite::agentspaces::Value());
  ASSERT_NE(event_node->state_kind(), velite::agentspaces::StateKind::Broken)
      << event_node->broken_reason();
  std::shared_ptr<velite::agentspaces::Handle> sub = event_node->ask(
      "subscribe", velite::agentspaces::Value::make_object(
                       {{"sink", velite::agentspaces::Value(sink)}}));
  ASSERT_NE(sub->state_kind(), velite::agentspaces::StateKind::Broken)
      << sub->broken_reason();

  ChromeRoot* root = CreateChromeRoot();
  ASSERT_NE(root, nullptr);
  DispatchOutcome outcome = RootDispatch(
      root, "targets/" + id + "/cdp/Runtime/evaluate/invoke",
      "{\"expression\":\"console.log('cdp-evt-42')\"}");
  ASSERT_EQ(outcome.kind, DispatchOutcome::Kind::kPending) << outcome.reply;
  ASSERT_TRUE(WaitUntilSettled(outcome.answer));

  ASSERT_TRUE(base::test::RunUntil([&]() { return !sink->frames().empty(); }))
      << "consoleAPICalled never arrived";
  bool saw_logged_text = false;
  for (const velite::agentspaces::Value& frame : sink->frames()) {
    const velite::agentspaces::Value* event = frame.object_get("event");
    ASSERT_NE(event, nullptr);
    EXPECT_EQ(event->as_string(), "Runtime.consoleAPICalled");
    if (SerializeWireReply(velite::agentspaces::ValueHandle::make(frame))
            .find("cdp-evt-42") != std::string::npos) {
      saw_logged_text = true;
    }
  }
  EXPECT_TRUE(saw_logged_text) << "no frame carried the logged text";
  DestroyChromeRoot(root);
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
