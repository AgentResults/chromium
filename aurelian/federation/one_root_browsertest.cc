// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// ACM-2w RED B (AURELIAN-GENERIC-CONTROL-TDD-PLAN; design section 5, round-5
// review M1): the counted-construction one-root audit. The fork's two wire
// bridge sites must consume the ONE ChromeDispatchFn exported over the
// INSTALLED sealed root; the WSS scaffold's lazy `static ChromeRoot*` was a
// SECOND consumption of ambient authority, disconnected from the install
// membrane — no media observable can distinguish the roots (both construct
// media_ over the process-global seam), so the audit counts constructions.
// RED: the count is 2 after a DispatchChromeRoot (the lazy static), and the
// exported accessor answers fail-closed. GREEN: one root, both sites.

#include <string>

#include "aurelian/bootstrap/browser_main_extra.h"
#include "aurelian/federation/wss_peer.h"
#include "aurelian/handles/root/root_handle.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "content/public/test/browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace aurelian {

class AurelianOneRootBrowserTest : public InProcessBrowserTest {};

IN_PROC_BROWSER_TEST_F(AurelianOneRootBrowserTest,
                       OneRootAcrossBothBridgeSites) {
  // The one-shot install at boot constructed the ONE sealed root.
  EXPECT_EQ(ChromeRootConstructionCountForTesting(), 1)
      << "the install must be the only root construction at boot";

  // The production path: the bootstrap's exported dispatch fn over the
  // INSTALLED root (the very fn the UDS register-in consumes).
  EXPECT_EQ(InstalledChromeDispatch()("__getIdentity", std::string()),
            "legion://chrome/")
      << "the exported dispatch must resolve against the installed root";

  // The WSS scaffold's entry point consumes the SAME fn (design section 5:
  // its lazy static second root is deleted).
  EXPECT_EQ(DispatchChromeRoot("__getIdentity"), "legion://chrome/");

  // The counted-construction audit: ONE root across both bridge sites.
  EXPECT_EQ(ChromeRootConstructionCountForTesting(), 1)
      << "a second ChromeRootHandle was constructed — a second consumption "
         "of ambient authority against EMBODIMENT-ONE-SHOT-CONSUMPTION "
         "(design section 5)";
}

}  // namespace aurelian
