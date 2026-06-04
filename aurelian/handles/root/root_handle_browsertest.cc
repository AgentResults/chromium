// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian browser tests — the navigable legion://chrome/ root reaches a real
// browser capability by walking down the tree (root -> system -> info).

#include "aurelian/handles/root/root_handle.h"

#include "aurelian/handles/browser/gpu_handle.h"
#include "aurelian/membrane/embodiment_policy.h"
#include "base/process/process.h"
#include "base/strings/string_number_conversions.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/test/browser_test.h"
#include "url/gurl.h"

namespace aurelian {

class AurelianRootBrowserTest : public InProcessBrowserTest {};

IN_PROC_BROWSER_TEST_F(AurelianRootBrowserTest, NavigatesToRealCapability) {
  ChromeRoot* root = CreateChromeRoot();
  ASSERT_NE(root, nullptr);

  // The root answers its identity.
  EXPECT_EQ(RootDispatch(root, "__getIdentity"), "legion://chrome/");

  // Walk root -> system -> info, reaching the REAL system capability: the
  // reported browser pid is this process.
  std::string info = RootDispatch(root, "system/info");
  EXPECT_NE(info.find("\"browserPid\":"), std::string::npos) << info;
  EXPECT_NE(info.find(std::to_string(base::Process::Current().Pid())),
            std::string::npos)
      << info;

  // An unknown child is broken.
  EXPECT_NE(RootDispatch(root, "bogus").find("broken"), std::string::npos);

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
  std::string count = RootDispatch(root, "tabs/count");
  int n = 0;
  ASSERT_TRUE(base::StringToInt(count, &n)) << "count not an int: " << count;
  EXPECT_GE(n, 1) << count;

  // The active tab's URL is reachable too and reflects where we navigated.
  std::string active = RootDispatch(root, "tabs/activeUrl");
  EXPECT_NE(active.find("data:text/html"), std::string::npos) << active;

  DestroyChromeRoot(root);
}

IN_PROC_BROWSER_TEST_F(AurelianRootBrowserTest, NavigatesToGpuCapability) {
  ChromeRoot* root = CreateChromeRoot();
  ASSERT_NE(root, nullptr);

  // Walk root -> gpu -> info, reaching the REAL GpuDataManager: the serialized
  // GL renderer matches an independent read (so it holds on any GPU).
  std::string info = RootDispatch(root, "gpu/info");
  EXPECT_NE(info.find("\"glRenderer\":"), std::string::npos) << info;
  EXPECT_NE(info.find(GetGpuSummary().gl_renderer), std::string::npos) << info;

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
  EXPECT_EQ(RootDispatch(root, "media/__getIdentity"), "legion://chrome/media");

  // Walk root -> media -> {camera,mic,peer-audio} -> describe: each advertises
  // its endowment contract (the bindable surface for Cicero §26).
  EXPECT_NE(RootDispatch(root, "media/camera/describe").find("video_sink"),
            std::string::npos)
      << RootDispatch(root, "media/camera/describe");
  EXPECT_NE(RootDispatch(root, "media/mic/describe").find("audio_sink"),
            std::string::npos)
      << RootDispatch(root, "media/mic/describe");
  EXPECT_NE(
      RootDispatch(root, "media/peer-audio/describe").find("audio_source"),
      std::string::npos)
      << RootDispatch(root, "media/peer-audio/describe");

  DestroyChromeRoot(root);
}

// The seal holds: a root whose policy does NOT grant `media` refuses it (no
// ambient media surface leaks to an unauthorised membrane).
IN_PROC_BROWSER_TEST_F(AurelianRootBrowserTest, MediaSurfaceSealedOff) {
  ChromeRoot* root =
      CreateChromeRootWithPolicy(EmbodimentPolicy::WithCapabilities({"system"}));
  ASSERT_NE(root, nullptr);

  EXPECT_NE(RootDispatch(root, "media").find("broken"), std::string::npos);
  EXPECT_NE(RootDispatch(root, "media/camera/describe").find("broken"),
            std::string::npos);

  DestroyChromeRoot(root);
}

}  // namespace aurelian
