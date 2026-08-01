// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian browser tests — the navigable legion://chrome/ root reaches a real
// browser capability by walking down the tree (root -> system -> info).
//
// ACM-R(5) (design section 7 FOLD rows): the kept facade verbs are semantic
// conveniences whose IMPLEMENTATIONS delegate to the mirror — a facade ask
// answers PENDING (settled by the HS-1 session layer) and attaches the ONE
// registry browser session. These tests own their waits (the
// cdp_session_browsertest.cc precedent); asserted VALUES are unchanged from
// the pre-repoint facade (the keystone wire shapes are preserved).

#include "aurelian/handles/root/root_handle.h"

#include <string>
#include <utility>

#include "aurelian/handles/root/wire_serialize.h"
#include "aurelian/membrane/embodiment_policy.h"
#include "aurelian/mirror/cdp_session.h"
#include "base/process/process.h"
#include "base/strings/string_number_conversions.h"
#include "base/test/run_until.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/gpu_data_manager.h"
#include "content/public/test/browser_test.h"
#include "gpu/config/gpu_info.h"
#include "url/gurl.h"
#include "velite/agentspaces-wire/handle.hpp"

namespace aurelian {

namespace {

using velite::agentspaces::StateKind;

// Settles a dispatch outcome to its wire reply: Completed serializes
// immediately (exactly the old path); Pending pumps the UI loop until the
// HS-1 session layer settles it — the TEST owns the wait, the production
// wire path waits on its completion record instead.
WireReply SettleToReply(DispatchOutcome outcome) {
  if (outcome.kind == DispatchOutcome::Kind::kCompleted) {
    return outcome.reply;
  }
  EXPECT_TRUE(base::test::RunUntil([&]() {
    return outcome.answer->state_kind() != StateKind::Pending;
  })) << "pending facade answer never settled";
  return SerializeWireReply(outcome.answer);
}

// The canonical-JSON payload of the settled reply. A refusal would surface
// here as its reason text, so tests that probe for a REFUSAL assert on
// SettleToReply(...).is_broken() rather than on this string.
std::string DispatchAndWait(ChromeRoot* root, const std::string& path) {
  return SettleToReply(RootDispatch(root, path)).payload;
}

}  // namespace

class AurelianRootBrowserTest : public InProcessBrowserTest {};

IN_PROC_BROWSER_TEST_F(AurelianRootBrowserTest, NavigatesToRealCapability) {
  ChromeRoot* root = CreateChromeRoot();
  ASSERT_NE(root, nullptr);

  // The root answers its identity.
  EXPECT_EQ(RootDispatch(root, "__getIdentity").reply.payload,
            "\"legion://chrome/\"");

  // Walk root -> system -> info, reaching the REAL system capability: the
  // reported browser pid is this process, and at least one live renderer is
  // counted (absorbed from the deleted system_handle_browsertest).
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>sys</title>sys")));
  std::string info = DispatchAndWait(root, "system/info");
  EXPECT_NE(info.find("\"browserPid\":"), std::string::npos) << info;
  EXPECT_NE(info.find(std::to_string(base::Process::Current().Pid())),
            std::string::npos)
      << info;
  EXPECT_NE(info.find("\"rendererCount\":"), std::string::npos) << info;
  EXPECT_EQ(info.find("\"rendererCount\":0"), std::string::npos) << info;

  // An unknown child is broken.
  EXPECT_TRUE(RootDispatch(root, "bogus").reply.is_broken());

  DestroyChromeRoot(root);
}

// ACM-R(5) RED: the facade verbs ride the mirror session — the ask answers
// PENDING (no bespoke synchronous read survives) and the first facade
// invoke lazily attaches the ONE registry browser session.
IN_PROC_BROWSER_TEST_F(AurelianRootBrowserTest, FacadeVerbsRideTheMirrorSession) {
  ChromeRoot* root = CreateChromeRoot();
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(CdpSessionRegistry::Get().SessionCountForTesting(), 0u);

  DispatchOutcome info = RootDispatch(root, "system/info");
  EXPECT_EQ(info.kind, DispatchOutcome::Kind::kPending)
      << "system/info did not go through the mirror session — a bespoke "
         "synchronous read survives: "
      << info.reply;
  std::string reply = SettleToReply(std::move(info)).payload;
  EXPECT_NE(reply.find("\"browserPid\":"), std::string::npos) << reply;
  EXPECT_EQ(CdpSessionRegistry::Get().SessionCountForTesting(), 1u)
      << "the facade invoke must lazily attach the ONE registry browser "
         "session (design section 3 residency — no parallel machinery)";

  DestroyChromeRoot(root);
}

IN_PROC_BROWSER_TEST_F(AurelianRootBrowserTest, NavigatesToTabsCapability) {
  // The test browser has a real, live tab; point it at a known URL.
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>root-tabs</title>")));

  ChromeRoot* root = CreateChromeRoot();
  ASSERT_NE(root, nullptr);

  // Walk root -> tabs -> count, reaching the REAL live tab strip: at least the
  // one tab this browser test opened.
  std::string count = DispatchAndWait(root, "tabs/count");
  int n = 0;
  ASSERT_TRUE(base::StringToInt(count, &n)) << "count not an int: " << count;
  EXPECT_GE(n, 1) << count;

  // The active tab's URL is reachable too and reflects where we navigated.
  std::string active = DispatchAndWait(root, "tabs/activeUrl");
  EXPECT_NE(active.find("data:text/html"), std::string::npos) << active;

  DestroyChromeRoot(root);
}

IN_PROC_BROWSER_TEST_F(AurelianRootBrowserTest, NavigatesToGpuCapability) {
  ChromeRoot* root = CreateChromeRoot();
  ASSERT_NE(root, nullptr);

  // Walk root -> gpu -> info, reaching the browser's REAL collected GPU info:
  // the serialized GL renderer matches an independent GpuDataManager read (so
  // it holds on any GPU; absorbed from the deleted gpu_handle_browsertest).
  gpu::GPUInfo gpu_info =
      content::GpuDataManager::GetInstance()->GetGPUInfo();
  std::string info = DispatchAndWait(root, "gpu/info");
  EXPECT_NE(info.find("\"glRenderer\":"), std::string::npos) << info;
  EXPECT_NE(info.find(gpu_info.gl_renderer), std::string::npos) << info;
  EXPECT_NE(info.find("\"vendorId\":"), std::string::npos) << info;

  DestroyChromeRoot(root);
}

// C-MEDIA-1b: the bindable media surface is reachable through the install-sealed
// root — the Cicero endowment binds to legion://chrome/media/{camera,mic,
// peer-audio}. RED before the root mounts `media` (out-of-scope); GREEN once the
// policy grants it and the root returns the persistent MediaHandle.
IN_PROC_BROWSER_TEST_F(AurelianRootBrowserTest, NavigatesToMediaSurface) {
  ChromeRoot* root = CreateChromeRoot();  // FullStandalone seals in `media`.
  ASSERT_NE(root, nullptr);

  // The media surface answers its identity through the sealed root.
  EXPECT_EQ(RootDispatch(root, "media/__getIdentity").reply.payload,
            "\"legion://chrome/media\"");

  // Walk root -> media -> {camera,mic,peer-audio} -> describe: each advertises
  // its endowment contract (the bindable surface for Cicero §26).
  EXPECT_NE(RootDispatch(root, "media/camera/describe").reply.payload.find(
                "video_sink"),
            std::string::npos)
      << RootDispatch(root, "media/camera/describe").reply;
  EXPECT_NE(RootDispatch(root, "media/mic/describe").reply.payload.find(
                "audio_sink"),
            std::string::npos)
      << RootDispatch(root, "media/mic/describe").reply;
  EXPECT_NE(
      RootDispatch(root, "media/peer-audio/describe").reply.payload.find(
          "audio_source"),
      std::string::npos)
      << RootDispatch(root, "media/peer-audio/describe").reply;

  DestroyChromeRoot(root);
}

// The seal holds: a root whose policy does NOT grant `media` refuses it (no
// ambient media surface leaks to an unauthorised membrane).
IN_PROC_BROWSER_TEST_F(AurelianRootBrowserTest, MediaSurfaceSealedOff) {
  ChromeRoot* root =
      CreateChromeRootWithPolicy(EmbodimentPolicy::WithCapabilities({"system"}));
  ASSERT_NE(root, nullptr);

  EXPECT_TRUE(RootDispatch(root, "media").reply.is_broken());
  EXPECT_TRUE(RootDispatch(root, "media/camera/describe").reply.is_broken());

  DestroyChromeRoot(root);
}

}  // namespace aurelian
