// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C-INSTALL browser test — the install-membrane SEAL.
//
// Per spec/facilities/embodiment.md §2 [EMBODIMENT-SEAL-NO-AMBIENT-PATH] and
// the conformance vector core/embodiment/v0.15/membrane-seal-no-ambient-path:
// after install returns, no un-wrapped path from inside the AgentSpace to the
// host's ambient (Chromium) surface is reachable. The only authority reachable
// from inside is what install wrapped as Handles. This probes the seal from
// the wire side (the property is wire-observable per
// legion://facets/embodiment/wire-install-membrane):
//   (a) a capability NOT in the policy is unreachable -> broken("out-of-scope")
//   (b) a wrapped Handle exposes no unwrap to its raw ambient ref
//       -> broken("unknown-message")
//   (c) the root yields no ambient authority directly -> broken("unknown-message")
// plus [EMBODIMENT-ONE-SHOT-CONSUMPTION] and [EMBODIMENT-MEMBRANE-REVOCABLE].

#include "aurelian/membrane/install.h"

#include <string>
#include <utility>

#include "aurelian/handles/root/root_handle.h"
#include "aurelian/handles/root/wire_serialize.h"
#include "aurelian/membrane/embodiment_policy.h"
#include "base/test/run_until.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "content/public/test/browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "velite/agentspaces-wire/agentspace.hpp"
#include "velite/agentspaces-wire/handle.hpp"

namespace aurelian {

namespace {
bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

// ACM-R(5): a granted facade verb settles through the HS-1 session layer;
// the membrane probes below stay synchronous (refusals never dispatch).
std::string DispatchAndWait(ChromeRoot* root, const std::string& path) {
  DispatchOutcome outcome = RootDispatch(root, path);
  if (outcome.kind == DispatchOutcome::Kind::kCompleted) {
    return outcome.reply;
  }
  EXPECT_TRUE(base::test::RunUntil([&]() {
    return outcome.answer->state_kind() !=
           velite::agentspaces::StateKind::Pending;
  })) << "pending facade answer never settled";
  return SerializeWireReply(outcome.answer);
}
}  // namespace

using AurelianInstallMembraneBrowserTest = InProcessBrowserTest;

IN_PROC_BROWSER_TEST_F(AurelianInstallMembraneBrowserTest, SealsAmbientSurface) {
  auto space = velite::agentspaces::AgentSpace::make("seal-test");

  // A policy that grants ONLY "system" — "tabs" and "gpu" are withheld.
  EmbodimentPolicy policy = EmbodimentPolicy::WithCapabilities({"system"});

  ChromeRoot* root = InstallChromeEmbodiment(*space, policy);
  ASSERT_NE(root, nullptr);

  // The granted capability is reachable through the membrane (real value).
  EXPECT_TRUE(Contains(DispatchAndWait(root, "system/info"), "browserPid"))
      << DispatchAndWait(root, "system/info");

  // (a) Capabilities outside the policy are unreachable — broken('out-of-scope'),
  //     not a live tab strip / GPU surface.
  EXPECT_TRUE(Contains(RootDispatch(root, "tabs/count").reply, "out-of-scope"))
      << RootDispatch(root, "tabs/count").reply;
  EXPECT_TRUE(Contains(RootDispatch(root, "gpu/info").reply, "out-of-scope"))
      << RootDispatch(root, "gpu/info").reply;
  // A capability the membrane does not mount at all is likewise out of scope.
  EXPECT_TRUE(Contains(RootDispatch(root, "net").reply, "out-of-scope"))
      << RootDispatch(root, "net").reply;

  // (b) The wrapped Handle exposes no unwrap to its raw ambient reference.
  EXPECT_TRUE(Contains(RootDispatch(root, "system/__ambient").reply, "unknown-message"))
      << RootDispatch(root, "system/__ambient").reply;

  // (c) The root yields no ambient authority directly.
  EXPECT_TRUE(Contains(RootDispatch(root, "__ambient").reply, "unknown-message"))
      << RootDispatch(root, "__ambient").reply;

  // [EMBODIMENT-ONE-SHOT-CONSUMPTION] — a second install on the SAME space is
  // refused (per-AgentSpace one-shot); the membrane is not re-opened.
  EXPECT_TRUE(IsEmbodimentInstalled(*space));
  EXPECT_EQ(InstallChromeEmbodiment(*space, policy), nullptr);

  // [EMBODIMENT-MEMBRANE-REVOCABLE] — destroying the AgentSpace revokes the
  // whole membrane.
  UninstallChromeEmbodiment(*space, root);
  EXPECT_TRUE(space->revoked());
  EXPECT_FALSE(IsEmbodimentInstalled(*space));
}

}  // namespace aurelian
