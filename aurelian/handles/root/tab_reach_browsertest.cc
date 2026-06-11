// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian AU-TAB-REACH: the INTERACTIVE tab verbs (open / activate / close +
// per-tab url) are reachable from the sealed root via RootDispatch — the SAME
// entry a remote controller walks — and they MUTATE the live tab strip.
// ACM-R(5): the interactive verbs delegate to the mirror sessions
// (Target.createTarget / Page.bringToFront / Target.closeTarget) with the
// strip-index resolution and the last-tab guard surviving as recorded glue
// (the inventory FOLD table); the asserted strip effects are unchanged. The
// tests own their settle waits; asserted values are the pre-repoint wire
// shapes (count int, "ok"/"refused"/"bad-index" scalars, the committed URL).

#include "aurelian/handles/root/root_handle.h"

#include <string>
#include <utility>

#include "aurelian/handles/root/wire_serialize.h"
#include "base/test/run_until.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/test/browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"
#include "velite/agentspaces-wire/handle.hpp"

namespace aurelian {

namespace {

using velite::agentspaces::StateKind;

std::string SettleToReply(DispatchOutcome outcome) {
  if (outcome.kind == DispatchOutcome::Kind::kCompleted) {
    return outcome.reply;
  }
  EXPECT_TRUE(base::test::RunUntil([&]() {
    return outcome.answer->state_kind() != StateKind::Pending;
  })) << "pending facade answer never settled";
  return SerializeWireReply(outcome.answer);
}

std::string DispatchAndWait(ChromeRoot* root, const std::string& path) {
  return SettleToReply(RootDispatch(root, path));
}

}  // namespace

using AurelianTabReachBrowserTest = InProcessBrowserTest;

IN_PROC_BROWSER_TEST_F(AurelianTabReachBrowserTest, ControllerDrivesLiveTabStrip) {
  ChromeRoot* root = CreateChromeRoot();

  // The browser starts with exactly one tab.
  EXPECT_EQ(DispatchAndWait(root, "tabs/count"), "1");

  // Closing the browser's last remaining tab is refused (a window close is
  // out of scope) — the per-browser last-tab guard survives as pre-dispatch
  // glue (absorbed from the deleted tabstrip_handle_browsertest).
  EXPECT_EQ(DispatchAndWait(root, "tabs/0/close"), "refused");
  EXPECT_EQ(DispatchAndWait(root, "tabs/count"), "1");

  // Open a second tab THROUGH the mounted interactive verb. The open
  // foregrounds it, so the new tab (index 1) becomes active and its index is
  // returned (the settle-time strip-index lookup).
  EXPECT_EQ(DispatchAndWait(root, "tabs/open"), "1")
      << "tabs/open is unreachable — the interactive tab handle is not mounted";
  EXPECT_EQ(DispatchAndWait(root, "tabs/count"), "2");
  EXPECT_EQ(DispatchAndWait(root, "tabs/activeIndex"), "1");

  // `activate` genuinely moves the active tab on the LIVE strip — both ways.
  EXPECT_EQ(DispatchAndWait(root, "tabs/0/activate"), "ok");
  EXPECT_EQ(DispatchAndWait(root, "tabs/activeIndex"), "0");
  EXPECT_EQ(DispatchAndWait(root, "tabs/1/activate"), "ok");
  EXPECT_EQ(DispatchAndWait(root, "tabs/activeIndex"), "1");

  // A bad index is refused without dispatching anything (index->target
  // resolution is the recorded selection glue).
  EXPECT_EQ(DispatchAndWait(root, "tabs/5/activate"), "bad-index");

  // The per-tab handle reads real committed state: navigate the active tab
  // (tab 1) to a data: URL and read it back through `tabs/1/url`.
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>reach</title>tab-reach")));
  EXPECT_EQ(DispatchAndWait(root, "tabs/1/url"),
            "data:text/html,<title>reach</title>tab-reach");

  // `close` shrinks the live strip.
  EXPECT_EQ(DispatchAndWait(root, "tabs/1/close"), "ok");
  EXPECT_EQ(DispatchAndWait(root, "tabs/count"), "1");

  DestroyChromeRoot(root);
}

}  // namespace aurelian
