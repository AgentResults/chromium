// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C13.d browser tests — profile info (incognito state + identity).

#include "aurelian/handles/browser/profile_handle.h"

#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "content/public/test/browser_test.h"

namespace aurelian {

using AurelianProfileBrowserTest = InProcessBrowserTest;

IN_PROC_BROWSER_TEST_F(AurelianProfileBrowserTest, ReportsIncognitoState) {
  // The default browser is a regular (non-OTR) profile.
  ProfileInfo regular = GetProfileInfo(browser()->profile());
  EXPECT_FALSE(regular.off_the_record);
  EXPECT_FALSE(regular.unique_id.empty());

  // An incognito browser is off-the-record, with a distinct identity.
  Browser* incognito = CreateIncognitoBrowser();
  ProfileInfo otr = GetProfileInfo(incognito->profile());
  EXPECT_TRUE(otr.off_the_record);
  EXPECT_FALSE(otr.unique_id.empty());
  EXPECT_NE(otr.unique_id, regular.unique_id);
}

}  // namespace aurelian
