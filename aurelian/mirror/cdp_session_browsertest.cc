// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// ACM-2 browser tests (AURELIAN-GENERIC-CONTROL-TDD-PLAN Phase A): invoke
// through the HS-1 async session layer, on the BROWSER target (design
// section 3 — the replacement for the devtools_handle.cc one-shot shape).
// In-process probes against the real browser: the dispatch returns a
// PENDING outcome, settlement arrives in a later UI turn (the test pumps
// its own loop — production code never does), two commands fly concurrently
// on the ONE persistent browser-target session and correlate to the right
// replies, page-scoped commands refuse typed at the browser mirror (design
// section 2: the browser session's width is CLOSED), and the first invoke
// attaches lazily.
//
// RED: RootDispatch carries DispatchOutcome but nothing creates a Pending
// answer — every invoke below answers Completed broken (no session layer,
// no session-context gate, SessionCountForTesting stuck at 0).

#include <memory>
#include <string>

#include "aurelian/handles/root/root_handle.h"
#include "aurelian/handles/root/wire_serialize.h"
#include "aurelian/mirror/cdp_session.h"
#include "base/json/json_reader.h"
#include "base/test/run_until.h"
#include "base/values.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/test/browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"
#include "velite/agentspaces-wire/handle.hpp"

namespace aurelian {

namespace {

using velite::agentspaces::Handle;
using velite::agentspaces::StateKind;

// Pumps the UI loop until `answer` settles (the TEST owns the wait — the
// production wire path waits on its completion record instead; in-process
// pumping here is exactly the proof that no production code blocks).
[[nodiscard]] bool WaitUntilSettled(const std::shared_ptr<Handle>& answer) {
  return base::test::RunUntil(
      [&]() { return answer->state_kind() != StateKind::Pending; });
}

std::optional<base::DictValue> ParseDict(const std::string& reply) {
  return base::JSONReader::ReadDict(reply, base::JSON_PARSE_RFC);
}

}  // namespace

class AurelianCdpSessionBrowserTest : public InProcessBrowserTest {};

// cdp/Browser/getVersion invoke {} -> a PENDING outcome whose answer
// settles to the real product/revision strings (the async layer's
// happy path; Browser is in the closed browser union, design section 2).
IN_PROC_BROWSER_TEST_F(AurelianCdpSessionBrowserTest,
                       InvokeBrowserGetVersionThroughAsyncLayer) {
  ChromeRoot* root = CreateChromeRoot();
  ASSERT_NE(root, nullptr);

  DispatchOutcome outcome = RootDispatch(root, "cdp/Browser/getVersion/invoke");
  ASSERT_EQ(outcome.kind, DispatchOutcome::Kind::kPending)
      << "not pending — the async layer is absent: " << outcome.reply;
  ASSERT_NE(outcome.answer, nullptr);

  ASSERT_TRUE(WaitUntilSettled(outcome.answer));
  ASSERT_EQ(outcome.answer->state_kind(), StateKind::ResolvedValue)
      << "broken: " << outcome.answer->broken_reason();

  std::string reply = SerializeWireReply(outcome.answer).payload;
  std::optional<base::DictValue> result = ParseDict(reply);
  ASSERT_TRUE(result.has_value()) << reply;
  const std::string* product = result->FindString("product");
  ASSERT_NE(product, nullptr) << reply;
  EXPECT_FALSE(product->empty());
  const std::string* user_agent = result->FindString("userAgent");
  ASSERT_NE(user_agent, nullptr) << reply;
  EXPECT_FALSE(user_agent->empty());

  DestroyChromeRoot(root);
}

// TWO commands in flight concurrently on the ONE browser-target session,
// each correlated to the right reply — impossible on the old single-id
// one-shot (this assert is what proves the replacement, plan ACM-2).
IN_PROC_BROWSER_TEST_F(AurelianCdpSessionBrowserTest,
                       TwoInFlightCorrelatedOnOneSession) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,acm2fixture")));

  ChromeRoot* root = CreateChromeRoot();
  ASSERT_NE(root, nullptr);

  DispatchOutcome version = RootDispatch(root, "cdp/Browser/getVersion/invoke");
  DispatchOutcome targets = RootDispatch(root, "cdp/Target/getTargets/invoke");
  ASSERT_EQ(version.kind, DispatchOutcome::Kind::kPending) << version.reply;
  ASSERT_EQ(targets.kind, DispatchOutcome::Kind::kPending) << targets.reply;

  // Both genuinely in flight, on ONE persistent session, before any pump.
  EXPECT_EQ(CdpSessionRegistry::Get().SessionCountForTesting(), 1u);
  EXPECT_EQ(CdpSessionRegistry::Get().InFlightCountForTesting(), 2u);

  ASSERT_TRUE(WaitUntilSettled(version.answer));
  ASSERT_TRUE(WaitUntilSettled(targets.answer));
  ASSERT_EQ(version.answer->state_kind(), StateKind::ResolvedValue)
      << version.answer->broken_reason();
  ASSERT_EQ(targets.answer->state_kind(), StateKind::ResolvedValue)
      << targets.answer->broken_reason();

  // Correlation: each answer is the RIGHT one (the old "id":1 matcher
  // would hand one reply to both, or the wrong reply to either).
  std::string version_reply = SerializeWireReply(version.answer).payload;
  std::string targets_reply = SerializeWireReply(targets.answer).payload;
  EXPECT_NE(version_reply.find("product"), std::string::npos)
      << version_reply;
  EXPECT_EQ(version_reply.find("targetInfos"), std::string::npos)
      << version_reply;
  EXPECT_NE(targets_reply.find("targetInfos"), std::string::npos)
      << targets_reply;
  EXPECT_EQ(targets_reply.find("userAgent"), std::string::npos)
      << targets_reply;
  // The open tab's target is in the enumeration (real browser state).
  EXPECT_NE(targets_reply.find("acm2fixture"), std::string::npos)
      << targets_reply;

  EXPECT_EQ(CdpSessionRegistry::Get().InFlightCountForTesting(), 0u);

  DestroyChromeRoot(root);
}

// An unknown command under a real domain answers a TYPED error — never a
// crash, never silence (catalog-lookup honesty; pinned from ACM-1, must
// survive the invoke wiring).
IN_PROC_BROWSER_TEST_F(AurelianCdpSessionBrowserTest, UnknownCommandTyped) {
  ChromeRoot* root = CreateChromeRoot();
  ASSERT_NE(root, nullptr);

  DispatchOutcome outcome =
      RootDispatch(root, "cdp/Browser/noSuchCommand/invoke");
  EXPECT_EQ(outcome.kind, DispatchOutcome::Kind::kCompleted);
  EXPECT_TRUE(outcome.reply.is_broken());
  EXPECT_EQ(outcome.reply.payload, "unknown-command-or-event");

  DestroyChromeRoot(root);
}

// A page-scoped command at the BROWSER mirror is the typed session-context
// refusal NAMING the per-target path (design section 2, normative: the
// browser session's width is CLOSED; never a raw CDP "wasn't found"
// passthrough, never a dispatch that fails by archaeology).
IN_PROC_BROWSER_TEST_F(AurelianCdpSessionBrowserTest,
                       PageScopedCommandRefusedAtBrowserMirror) {
  ChromeRoot* root = CreateChromeRoot();
  ASSERT_NE(root, nullptr);

  DispatchOutcome outcome = RootDispatch(root, "cdp/Page/navigate/invoke");
  ASSERT_EQ(outcome.kind, DispatchOutcome::Kind::kCompleted);
  EXPECT_TRUE(outcome.reply.is_broken()) << outcome.reply;
  EXPECT_EQ(outcome.reply.payload.rfind("session-context", 0), 0u)
      << outcome.reply;
  EXPECT_NE(outcome.reply.payload.find("targets/<id>/cdp/Page/navigate"),
            std::string::npos)
      << outcome.reply;

  // The refusal is the gate's, not the host's: nothing attached, nothing
  // dispatched.
  EXPECT_EQ(CdpSessionRegistry::Get().SessionCountForTesting(), 0u);
  EXPECT_EQ(CdpSessionRegistry::Get().InFlightCountForTesting(), 0u);

  DestroyChromeRoot(root);
}

// HS-3 (ACM-2w): the spec reaches the FINAL hop's ask through
// RootDispatch's serialized-spec seam (design section 5 — intermediate hops
// stay nullary navigation). getFeatureState REQUIRES a parameter: a dropped
// spec is the host's own invalid-params error, never a result — so a
// featureEnabled result proves the spec crossed every layer to the host.
IN_PROC_BROWSER_TEST_F(AurelianCdpSessionBrowserTest,
                       InvokeCarriesSpecToFinalHop) {
  ChromeRoot* root = CreateChromeRoot();
  ASSERT_NE(root, nullptr);

  DispatchOutcome outcome =
      RootDispatch(root, "cdp/SystemInfo/getFeatureState/invoke",
                   "{\"featureState\":\"DIPS\"}");
  ASSERT_EQ(outcome.kind, DispatchOutcome::Kind::kPending) << outcome.reply;
  ASSERT_TRUE(WaitUntilSettled(outcome.answer));
  ASSERT_EQ(outcome.answer->state_kind(), StateKind::ResolvedValue)
      << "the spec did not reach the host: "
      << outcome.answer->broken_reason();
  std::string reply = SerializeWireReply(outcome.answer).payload;
  EXPECT_NE(reply.find("featureEnabled"), std::string::npos) << reply;

  // A MALFORMED serialized spec is a typed refusal, never a silent
  // empty-spec dispatch.
  DispatchOutcome bad = RootDispatch(
      root, "cdp/SystemInfo/getFeatureState/invoke", "{not json");
  EXPECT_EQ(bad.kind, DispatchOutcome::Kind::kCompleted);
  EXPECT_TRUE(bad.reply.is_broken()) << bad.reply;
  EXPECT_EQ(bad.reply.payload, "spec-parse-failure") << bad.reply;

  DestroyChromeRoot(root);
}

// Lazy attach on the first invoke (plan ACM-2, review N8's testable half:
// the browser target never closes, so detach-on-close rides ACM-3): no
// session before the first invoke; ONE persistent session after it; the
// second invoke reuses it (no attach-wait-detach churn).
IN_PROC_BROWSER_TEST_F(AurelianCdpSessionBrowserTest, LazyAttachOnFirstInvoke) {
  ChromeRoot* root = CreateChromeRoot();
  ASSERT_NE(root, nullptr);

  EXPECT_EQ(CdpSessionRegistry::Get().SessionCountForTesting(), 0u);

  DispatchOutcome first = RootDispatch(root, "cdp/Browser/getVersion/invoke");
  ASSERT_EQ(first.kind, DispatchOutcome::Kind::kPending) << first.reply;
  EXPECT_EQ(CdpSessionRegistry::Get().SessionCountForTesting(), 1u);
  ASSERT_TRUE(WaitUntilSettled(first.answer));

  DispatchOutcome second = RootDispatch(root, "cdp/Browser/getVersion/invoke");
  ASSERT_EQ(second.kind, DispatchOutcome::Kind::kPending) << second.reply;
  EXPECT_EQ(CdpSessionRegistry::Get().SessionCountForTesting(), 1u);
  ASSERT_TRUE(WaitUntilSettled(second.answer));
  EXPECT_EQ(second.answer->state_kind(), StateKind::ResolvedValue);

  DestroyChromeRoot(root);
}

}  // namespace aurelian
