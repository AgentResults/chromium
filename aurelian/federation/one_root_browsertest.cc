// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// ACM-2w RED B (AURELIAN-GENERIC-CONTROL-TDD-PLAN; design section 5, round-5
// review M1): the counted-construction one-root audit. Every wire bridge
// site must consume the ONE ChromeDispatchFn exported over the INSTALLED
// sealed root; the WSS scaffold's lazy `static ChromeRoot*` was a SECOND
// consumption of ambient authority, disconnected from the install membrane —
// no media observable can distinguish the roots (both construct media_ over
// the process-global seam), so the audit counts constructions. The original
// RED counted 2 after a DispatchChromeRoot (the lazy static); ACM-2w unified
// both sites on the exported fn, and ACM-R(3) deleted the scaffold outright
// (design section 7 DELETE-default row) — the surviving pin holds the one
// production bring-up (UDS register-in) to exactly one root construction.

#include <string>

#include "aurelian/bootstrap/browser_main_extra.h"
#include "aurelian/federation/completion_bridge.h"
#include "aurelian/handles/root/root_handle.h"
#include "base/test/run_until.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "content/public/test/browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace aurelian {

class AurelianOneRootBrowserTest : public InProcessBrowserTest {};

IN_PROC_BROWSER_TEST_F(AurelianOneRootBrowserTest, OneRootOneBridgeSite) {
  // The one-shot install at boot constructed the ONE sealed root.
  EXPECT_EQ(ChromeRootConstructionCountForTesting(), 1)
      << "the install must be the only root construction at boot";

  // The production path: the bootstrap's exported BEGIN-form dispatch over
  // the INSTALLED root (the very fn both wire bring-ups consume) — CF-6
  // TEST-CHANGE: the begin-form posts and the record settles via the HS-1
  // layer (the test pumps; same assertion substance).
  std::shared_ptr<CompletionRecord> record =
      InstalledBeginChromeDispatch()("__getIdentity", std::string());
  ASSERT_TRUE(record);
  ASSERT_TRUE(base::test::RunUntil([&]() {
    return record->outcome.load() != CompletionRecord::kPending;
  }));
  EXPECT_EQ(record->reply, "legion://chrome/")
      << "the exported dispatch must resolve against the installed root";

  // The counted-construction audit: dispatching through the exported fn
  // constructs nothing — ONE root, ONE bridge site (UDS register-in).
  EXPECT_EQ(ChromeRootConstructionCountForTesting(), 1)
      << "a second ChromeRootHandle was constructed — a second consumption "
         "of ambient authority against EMBODIMENT-ONE-SHOT-CONSUMPTION "
         "(design section 5)";
}

}  // namespace aurelian
