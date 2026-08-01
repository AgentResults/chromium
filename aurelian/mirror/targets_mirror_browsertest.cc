// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// ACM-3 browser tests (AURELIAN-GENERIC-CONTROL-TDD-PLAN Phase A): the
// targets mirror + the per-target sub-mirror — the page-scoped invokes
// land HERE (design section 2 session model, review F1). In-process
// probes against the real browser:
//
//  - `targets` __getChildren enumerates the REAL tab set via
//    DevToolsAgentHost::GetOrCreateAll() (NOT GetAll(), review F3: a tab
//    that never had a DevTools session must still appear — the test gets
//    the new tab's identity ONLY through the mirror, so a GetAll()-based
//    enumeration cannot pass).
//  - targets/<id>/cdp/Runtime/evaluate invokes on THAT tab (42 + the
//    tab's own location); targets/<id>/cdp/Page/navigate drives THAT tab
//    and not the other.
//  - The symmetric refusal half (design section 2): every DERIVED
//    browser-only domain refuses at a page sub-mirror NAMING the browser
//    path; SystemInfo.getInfo refuses at COMMAND level by the authored
//    override (its live-host pin is ACM-S2's
//    OverrideGetInfoRefusedOnPageSession, still in the regression suite)
//    while un-overridden getFeatureState DISPATCHES — the mirror is
//    exactly as wide as the host.
//  - Non-page targets (the tab target GetOrCreateAll also enumerates) are
//    annotation-not-applicable and dispatch-and-pass-through: NO derived
//    refusals; the host self-answers method-not-found (design section 2
//    round-8).
//  - Detach-on-target-close (review N8): an ARRANGED in-flight command
//    (awaitPromise on a never-settling promise — without arranging one
//    the in-flight-Broken assert is vacuously green) settles Broken when
//    the tab closes, and the persistent client is cleaned up.
//
// RED: nothing mounts `targets` — every dispatch below answers
// broken:out-of-scope, and TargetSessionCountForTesting is stuck at 0.

#include <set>
#include <string>
#include <vector>

#include "aurelian/catalog/cdp_catalog.h"
#include "aurelian/handles/root/root_handle.h"
#include "aurelian/handles/root/wire_serialize.h"
#include "aurelian/membrane/embodiment_policy.h"
#include "aurelian/mirror/cdp_session.h"
#include "base/json/json_reader.h"
#include "base/test/run_until.h"
#include "base/values.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_tabstrip.h"
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

using velite::agentspaces::Handle;
using velite::agentspaces::StateKind;

[[nodiscard]] bool WaitUntilSettled(const std::shared_ptr<Handle>& answer) {
  return base::test::RunUntil(
      [&]() { return answer->state_kind() != StateKind::Pending; });
}

std::optional<base::ListValue> ParseList(const std::string& reply) {
  return base::JSONReader::ReadList(reply, base::JSON_PARSE_RFC);
}

std::optional<base::DictValue> ParseDict(const std::string& reply) {
  return base::JSONReader::ReadDict(reply, base::JSON_PARSE_RFC);
}

std::set<std::string> AsStringSet(const base::ListValue& list) {
  std::set<std::string> out;
  for (const base::Value& v : list) {
    if (v.is_string()) {
      out.insert(v.GetString());
    }
  }
  return out;
}

constexpr char kTargetsPrefix[] = "legion://chrome/targets/";

}  // namespace

class AurelianTargetsMirrorBrowserTest : public InProcessBrowserTest {
 protected:
  content::WebContents* GetWC() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }

  // The active tab's PAGE-target id, via the host's own identity (test
  // tier; the enumeration asserts get identities through the mirror).
  std::string ActivePageTargetId() {
    return content::DevToolsAgentHost::GetOrCreateFor(GetWC())->GetId();
  }
};

// `targets` __getChildren enumerates the real tab set, and the catalog
// GROWS when a tab is opened through the facade. The new tab's identity
// is asserted ONLY through the mirror (no content:: call for it), so an
// enumeration that misses never-attached tabs — GetAll() — cannot pass.
IN_PROC_BROWSER_TEST_F(AurelianTargetsMirrorBrowserTest,
                       EnumerationGrowsWithRealTabSet) {
  ChromeRoot* root = CreateChromeRoot();
  ASSERT_NE(root, nullptr);

  EXPECT_EQ(RootDispatch(root, "targets/__getIdentity").reply.payload,
            "\"legion://chrome/targets\"");

  std::string before_reply = RootDispatch(root, "targets/__getChildren").reply.payload;
  std::optional<base::ListValue> before = ParseList(before_reply);
  ASSERT_TRUE(before.has_value()) << "not a JSON list: " << before_reply;
  for (const base::Value& c : *before) {
    ASSERT_TRUE(c.is_string()) << before_reply;
    EXPECT_EQ(c.GetString().rfind(kTargetsPrefix, 0), 0u) << c.GetString();
  }
  std::set<std::string> before_set = AsStringSet(*before);

  // Open a second tab directly on the strip (about:blank, foreground).
  // NOT via the facade: ACM-R(5) folded tabs/open onto Target.createTarget,
  // which MINTS a DevTools host for the new tab — the F3 pin here needs a
  // tab that genuinely never had one, so a GetAll()-based enumeration
  // still cannot pass.
  chrome::AddTabAt(browser(), GURL("about:blank"), /*index=*/-1,
                   /*foreground=*/true);
  ASSERT_EQ(browser()->tab_strip_model()->count(), 2);

  std::string after_reply = RootDispatch(root, "targets/__getChildren").reply.payload;
  std::optional<base::ListValue> after = ParseList(after_reply);
  ASSERT_TRUE(after.has_value()) << "not a JSON list: " << after_reply;
  std::set<std::string> after_set = AsStringSet(*after);

  // The catalog grew and lost nothing.
  EXPECT_GT(after_set.size(), before_set.size());
  for (const std::string& uri : before_set) {
    EXPECT_TRUE(after_set.count(uri)) << "enumeration lost " << uri;
  }

  // Tab-shaped asserts filter on `page` (design section 4: the mirror
  // reflects what Chromium reports — workers and the tab targets are
  // enumerated too, typed). Exactly ONE new page target: the added tab.
  std::vector<std::string> new_page_ids;
  for (const std::string& uri : after_set) {
    if (before_set.count(uri)) {
      continue;
    }
    std::string id = uri.substr(std::string(kTargetsPrefix).size());
    EXPECT_EQ(RootDispatch(root, "targets/" + id + "/__getType").reply.payload,
              "\"legion://types/ChromeTarget\"");
    std::string schema_reply =
        RootDispatch(root, "targets/" + id + "/__getSchema").reply.payload;
    std::optional<base::DictValue> schema = ParseDict(schema_reply);
    ASSERT_TRUE(schema.has_value()) << schema_reply;
    const std::string* type = schema->FindString("type");
    ASSERT_NE(type, nullptr) << schema_reply;
    if (*type == "page") {
      const std::string* url = schema->FindString("url");
      ASSERT_NE(url, nullptr) << schema_reply;
      EXPECT_NE(url->find("about:blank"), std::string::npos) << *url;
      new_page_ids.push_back(id);
    }
  }
  EXPECT_EQ(new_page_ids.size(), 1u)
      << "expected exactly one new page target (the added tab): "
      << after_reply;

  DestroyChromeRoot(root);
}

// targets/<id>/cdp/Runtime/evaluate invokes on THAT tab: 6*7 -> 42, and
// the tab's own location proves the session is target-scoped (the
// page-scoped happy path that ACM-2's browser mirror correctly refused).
IN_PROC_BROWSER_TEST_F(AurelianTargetsMirrorBrowserTest,
                       RuntimeEvaluateOnThatTab) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>acm3</title>acm3-evaluate-tab")));
  std::string id = ActivePageTargetId();

  ChromeRoot* root = CreateChromeRoot();
  ASSERT_NE(root, nullptr);

  DispatchOutcome outcome =
      RootDispatch(root, "targets/" + id + "/cdp/Runtime/evaluate/invoke",
                   "{\"expression\":\"6*7\"}");
  ASSERT_EQ(outcome.kind, DispatchOutcome::Kind::kPending)
      << "not pending — no per-target session layer: " << outcome.reply;
  ASSERT_TRUE(WaitUntilSettled(outcome.answer));
  ASSERT_EQ(outcome.answer->state_kind(), StateKind::ResolvedValue)
      << outcome.answer->broken_reason();
  std::string reply = SerializeWireReply(outcome.answer).payload;
  std::optional<base::DictValue> result = ParseDict(reply);
  ASSERT_TRUE(result.has_value()) << reply;
  EXPECT_EQ(result->FindIntByDottedPath("result.value").value_or(0), 42)
      << reply;

  // The session is THAT tab's: its own location comes back.
  DispatchOutcome loc =
      RootDispatch(root, "targets/" + id + "/cdp/Runtime/evaluate/invoke",
                   "{\"expression\":\"location.href\"}");
  ASSERT_EQ(loc.kind, DispatchOutcome::Kind::kPending) << loc.reply;
  ASSERT_TRUE(WaitUntilSettled(loc.answer));
  ASSERT_EQ(loc.answer->state_kind(), StateKind::ResolvedValue)
      << loc.answer->broken_reason();
  EXPECT_NE(SerializeWireReply(loc.answer).payload.find("acm3-evaluate-tab"),
            std::string::npos)
      << SerializeWireReply(loc.answer);

  DestroyChromeRoot(root);
}

// targets/<id>/cdp/Page/navigate drives THAT tab and not the other —
// both tabs' URLs asserted.
IN_PROC_BROWSER_TEST_F(AurelianTargetsMirrorBrowserTest,
                       PageNavigateDrivesThatTabOnly) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>t0</title>acm3-keep-tab")));
  content::WebContents* first_wc = GetWC();
  std::string first_url = first_wc->GetLastCommittedURL().spec();

  ChromeRoot* root = CreateChromeRoot();
  ASSERT_NE(root, nullptr);

  // Second tab via the facade (foreground -> active), then give it a
  // distinct starting page.
  EXPECT_NE(RootDispatch(root, "tabs/open").reply.payload, "-1");
  ASSERT_TRUE(base::test::RunUntil([&]() { return GetWC() != first_wc; }));
  content::WebContents* second_wc = GetWC();
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>t1</title>acm3-drive-tab")));
  std::string second_id =
      content::DevToolsAgentHost::GetOrCreateFor(second_wc)->GetId();

  content::TestNavigationObserver nav_observer(second_wc);
  DispatchOutcome outcome = RootDispatch(
      root, "targets/" + second_id + "/cdp/Page/navigate/invoke",
      "{\"url\":\"data:text/html,<title>t1n</title>acm3-navigated\"}");
  ASSERT_EQ(outcome.kind, DispatchOutcome::Kind::kPending)
      << "not pending — no per-target session layer: " << outcome.reply;
  ASSERT_TRUE(WaitUntilSettled(outcome.answer));
  ASSERT_EQ(outcome.answer->state_kind(), StateKind::ResolvedValue)
      << outcome.answer->broken_reason();
  nav_observer.Wait();

  // THAT tab navigated; the other did not.
  EXPECT_NE(
      second_wc->GetLastCommittedURL().spec().find("acm3-navigated"),
      std::string::npos)
      << second_wc->GetLastCommittedURL().spec();
  EXPECT_EQ(first_wc->GetLastCommittedURL().spec(), first_url);

  DestroyChromeRoot(root);
}

// The symmetric refusal half, domain level: every domain in the DERIVED
// closed browser-only set (membership named ONLY by the ACM-S2
// derivation + live pins — the set MAY be empty, in which case the
// command-level case below carries the refusal RED) refuses at a page
// sub-mirror with the typed session-context refusal NAMING the browser
// path. Mirror-side: no session attached, nothing dispatched.
IN_PROC_BROWSER_TEST_F(AurelianTargetsMirrorBrowserTest,
                       BrowserOnlyDomainsRefusedAtPageSubMirror) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>bo</title>acm3-refusal-tab")));
  std::string id = ActivePageTargetId();

  ChromeRoot* root = CreateChromeRoot();
  ASSERT_NE(root, nullptr);

  const CdpCatalog& cat = CdpCatalog::Get();
  std::vector<std::string> browser_only = cat.BrowserOnlyDomains();
  if (browser_only.empty()) {
    SUCCEED() << "derived browser-only set is empty (recorded; the "
                 "command-level override case carries the refusal RED)";
    DestroyChromeRoot(root);
    return;
  }

  for (const std::string& domain : browser_only) {
    std::optional<std::string> cmd = cat.FirstCommandOf(domain);
    ASSERT_TRUE(cmd.has_value()) << domain << " has no commands";
    DispatchOutcome outcome = RootDispatch(
        root, "targets/" + id + "/cdp/" + domain + "/" + *cmd + "/invoke");
    ASSERT_EQ(outcome.kind, DispatchOutcome::Kind::kCompleted)
        << domain << "." << *cmd << " dispatched — the gate is absent";
    EXPECT_TRUE(outcome.reply.is_broken());
    EXPECT_EQ(outcome.reply.payload.rfind("session-context", 0), 0u)
        << domain << "." << *cmd << ": " << outcome.reply;
    // The refusal NAMES the browser path (typed redirect, never protocol
    // archaeology — design section 2).
    EXPECT_NE(outcome.reply.payload.find("cdp/" + domain + "/" + *cmd),
              std::string::npos)
        << outcome.reply;
  }

  // The refusals were the mirror's: nothing attached.
  EXPECT_EQ(CdpSessionRegistry::Get().TargetSessionCountForTesting(), 0u);

  DestroyChromeRoot(root);
}

// The symmetric refusal half, command level: SystemInfo.getInfo refuses
// at the page sub-mirror by the AUTHORED override row, NAMING
// cdp/SystemInfo/getInfo — while the un-overridden sibling
// getFeatureState DISPATCHES on the same sub-mirror (the mirror is
// exactly as wide as the host). The override row's live-host pin —
// the raw page session yielding the host's own in-handler refusal — is
// ACM-S2's OverrideGetInfoRefusedOnPageSession, in the regression suite.
IN_PROC_BROWSER_TEST_F(AurelianTargetsMirrorBrowserTest,
                       OverrideRefusesAtCommandLevelSiblingDispatches) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>ov</title>acm3-override-tab")));
  std::string id = ActivePageTargetId();

  ChromeRoot* root = CreateChromeRoot();
  ASSERT_NE(root, nullptr);

  DispatchOutcome refused = RootDispatch(
      root, "targets/" + id + "/cdp/SystemInfo/getInfo/invoke");
  ASSERT_EQ(refused.kind, DispatchOutcome::Kind::kCompleted)
      << "getInfo dispatched — the command-level gate is absent";
  EXPECT_TRUE(refused.reply.is_broken());
  EXPECT_EQ(refused.reply.payload.rfind("session-context", 0), 0u)
      << refused.reply;
  EXPECT_NE(refused.reply.payload.find("cdp/SystemInfo/getInfo"), std::string::npos)
      << refused.reply;

  DispatchOutcome dispatched = RootDispatch(
      root, "targets/" + id + "/cdp/SystemInfo/getFeatureState/invoke",
      "{\"featureState\":\"DIPS\"}");
  ASSERT_EQ(dispatched.kind, DispatchOutcome::Kind::kPending)
      << "getFeatureState did not dispatch: " << dispatched.reply;
  ASSERT_TRUE(WaitUntilSettled(dispatched.answer));
  ASSERT_EQ(dispatched.answer->state_kind(), StateKind::ResolvedValue)
      << dispatched.answer->broken_reason();
  EXPECT_NE(SerializeWireReply(dispatched.answer).payload.find("featureEnabled"),
            std::string::npos)
      << SerializeWireReply(dispatched.answer);

  DestroyChromeRoot(root);
}

// Non-page sub-mirrors are annotation-not-applicable and
// dispatch-and-pass-through with NO derived refusals (design section 2
// round-8): the tab target's session is narrow and self-describing by
// answer — Runtime.evaluate passes the host's own method-not-found
// through (NEVER a session-context refusal), while Target.getTargets
// (in the tab session's own set) dispatches.
IN_PROC_BROWSER_TEST_F(AurelianTargetsMirrorBrowserTest,
                       NonPageTargetDispatchAndPassThrough) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>np</title>acm3-tab-target")));
  std::string tab_id =
      content::DevToolsAgentHost::GetOrCreateForTab(GetWC())->GetId();

  ChromeRoot* root = CreateChromeRoot();
  ASSERT_NE(root, nullptr);

  // The tab target is enumerated and typed (GetOrCreateAll includes it).
  std::string children_reply =
      RootDispatch(root, "targets/__getChildren").reply.payload;
  std::optional<base::ListValue> children = ParseList(children_reply);
  ASSERT_TRUE(children.has_value()) << children_reply;
  EXPECT_TRUE(AsStringSet(*children).count(kTargetsPrefix + tab_id))
      << children_reply;
  std::string schema_reply =
      RootDispatch(root, "targets/" + tab_id + "/__getSchema").reply.payload;
  std::optional<base::DictValue> schema = ParseDict(schema_reply);
  ASSERT_TRUE(schema.has_value()) << schema_reply;
  EXPECT_EQ(*schema->FindString("type"), "tab") << schema_reply;

  // Annotation-not-applicable on the non-page sub-mirror's domains.
  std::string domain_schema_reply =
      RootDispatch(root, "targets/" + tab_id + "/cdp/Runtime/__getSchema")
          .reply.payload;
  std::optional<base::DictValue> domain_schema =
      ParseDict(domain_schema_reply);
  ASSERT_TRUE(domain_schema.has_value()) << domain_schema_reply;
  EXPECT_EQ(domain_schema->FindBoolByDottedPath("sessionContext.notApplicable")
                .value_or(false),
            true)
      << domain_schema_reply;

  // NO derived refusal: the host self-answers method-not-found (-32601),
  // passed through typed.
  DispatchOutcome evaluate = RootDispatch(
      root, "targets/" + tab_id + "/cdp/Runtime/evaluate/invoke",
      "{\"expression\":\"1\"}");
  ASSERT_EQ(evaluate.kind, DispatchOutcome::Kind::kPending)
      << "not pending — no per-target session layer: " << evaluate.reply;
  ASSERT_TRUE(WaitUntilSettled(evaluate.answer));
  ASSERT_EQ(evaluate.answer->state_kind(), StateKind::Broken);
  std::string reason(evaluate.answer->broken_reason());
  EXPECT_EQ(reason.rfind("cdp-error:", 0), 0u)
      << "expected the HOST's own answer passed through, got: " << reason;
  EXPECT_NE(reason.find("-32601"), std::string::npos) << reason;
  EXPECT_EQ(reason.find("session-context"), std::string::npos)
      << "a derived refusal on a non-page sub-mirror: " << reason;

  // And what the tab session DOES serve dispatches.
  DispatchOutcome get_targets = RootDispatch(
      root, "targets/" + tab_id + "/cdp/Target/getTargets/invoke");
  ASSERT_EQ(get_targets.kind, DispatchOutcome::Kind::kPending)
      << get_targets.reply;
  ASSERT_TRUE(WaitUntilSettled(get_targets.answer));
  ASSERT_EQ(get_targets.answer->state_kind(), StateKind::ResolvedValue)
      << get_targets.answer->broken_reason();
  EXPECT_NE(SerializeWireReply(get_targets.answer).payload.find("targetInfos"),
            std::string::npos)
      << SerializeWireReply(get_targets.answer);

  DestroyChromeRoot(root);
}

// Detach-on-target-close (design section 3, review N8): ARRANGE a
// genuinely in-flight command (awaitPromise on a never-settling promise),
// close the tab, and the in-flight entry settles Broken while the
// persistent client is cleaned up.
IN_PROC_BROWSER_TEST_F(AurelianTargetsMirrorBrowserTest,
                       DetachOnTargetCloseSettlesInFlightBroken) {
  ChromeRoot* root = CreateChromeRoot();
  ASSERT_NE(root, nullptr);

  // A second tab to close (the first stays — CloseTabGlobal-style guard
  // is irrelevant here, the strip closes index 1 directly).
  content::WebContents* first_wc = GetWC();
  EXPECT_NE(RootDispatch(root, "tabs/open").reply.payload, "-1");
  ASSERT_TRUE(base::test::RunUntil([&]() { return GetWC() != first_wc; }));
  content::WebContents* second_wc = GetWC();
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>cl</title>acm3-close-tab")));
  std::string id =
      content::DevToolsAgentHost::GetOrCreateFor(second_wc)->GetId();

  EXPECT_EQ(CdpSessionRegistry::Get().TargetSessionCountForTesting(), 0u);

  DispatchOutcome in_flight = RootDispatch(
      root, "targets/" + id + "/cdp/Runtime/evaluate/invoke",
      "{\"expression\":\"new Promise(()=>{})\",\"awaitPromise\":true}");
  ASSERT_EQ(in_flight.kind, DispatchOutcome::Kind::kPending)
      << "not pending — no per-target session layer: " << in_flight.reply;
  EXPECT_EQ(CdpSessionRegistry::Get().TargetSessionCountForTesting(), 1u);

  // Genuinely in flight: still Pending after the send turn ran.
  EXPECT_EQ(in_flight.answer->state_kind(), StateKind::Pending);

  // Close THAT tab. The session detaches; the in-flight registrar entry
  // settles Broken (design section 3 session-teardown rule); the client
  // is cleaned up.
  int second_index =
      browser()->tab_strip_model()->GetIndexOfWebContents(second_wc);
  ASSERT_NE(second_index, TabStripModel::kNoTab);
  browser()->tab_strip_model()->CloseWebContentsAt(
      second_index, TabCloseTypes::CLOSE_USER_GESTURE);

  ASSERT_TRUE(WaitUntilSettled(in_flight.answer));
  EXPECT_EQ(in_flight.answer->state_kind(), StateKind::Broken);
  EXPECT_NE(std::string(in_flight.answer->broken_reason())
                .find("cdp-session-closed"),
            std::string::npos)
      << in_flight.answer->broken_reason();
  ASSERT_TRUE(base::test::RunUntil([&]() {
    return CdpSessionRegistry::Get().TargetSessionCountForTesting() == 0u;
  })) << "the persistent client was not cleaned up";

  DestroyChromeRoot(root);
}

// This slice's EmbodimentPolicy row (per-slice policy growth, design
// section 7 review N4): `targets` is sealed behind the membrane like
// every mountable — absent from the policy, the whole subtree refuses.
IN_PROC_BROWSER_TEST_F(AurelianTargetsMirrorBrowserTest,
                       TargetsSealedBehindPolicy) {
  ChromeRoot* sealed_off = CreateChromeRootWithPolicy(
      EmbodimentPolicy::WithCapabilities({"system"}));
  ASSERT_NE(sealed_off, nullptr);
  EXPECT_TRUE(RootDispatch(sealed_off, "targets").reply.is_broken());
  EXPECT_TRUE(
      RootDispatch(sealed_off, "targets/__getChildren").reply.is_broken());
  DestroyChromeRoot(sealed_off);

  ChromeRoot* full = CreateChromeRoot();
  ASSERT_NE(full, nullptr);
  std::string reply = RootDispatch(full, "targets/__getChildren").reply.payload;
  EXPECT_TRUE(ParseList(reply).has_value())
      << "FullStandalone does not mount targets: " << reply;
  DestroyChromeRoot(full);
}

}  // namespace aurelian
