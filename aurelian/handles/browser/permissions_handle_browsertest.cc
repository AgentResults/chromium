// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C7.c browser tests — permissions query / grant / revoke.

#include "aurelian/handles/browser/permissions_handle.h"

#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "content/public/test/browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace aurelian {

class AurelianPermBrowserTest : public InProcessBrowserTest {
 protected:
  HostContentSettingsMap* Map() {
    return HostContentSettingsMapFactory::GetForProfile(browser()->profile());
  }
};

IN_PROC_BROWSER_TEST_F(AurelianPermBrowserTest, QueryGrantRevoke) {
  const std::string origin = "https://perm.example.com";

  // Default geolocation state is "ask".
  EXPECT_EQ(QueryPermission(Map(), origin, "geolocation"), "ask");

  // Grant → allow.
  EXPECT_TRUE(GrantPermission(Map(), origin, "geolocation"));
  EXPECT_EQ(QueryPermission(Map(), origin, "geolocation"), "allow");

  // Revoke → block.
  EXPECT_TRUE(RevokePermission(Map(), origin, "geolocation"));
  EXPECT_EQ(QueryPermission(Map(), origin, "geolocation"), "block");

  // A second permission type is independent.
  EXPECT_TRUE(GrantPermission(Map(), origin, "notifications"));
  EXPECT_EQ(QueryPermission(Map(), origin, "notifications"), "allow");
  EXPECT_EQ(QueryPermission(Map(), origin, "geolocation"), "block");

  // Unknown permission is rejected.
  EXPECT_EQ(QueryPermission(Map(), origin, "telepathy"), "unknown-permission");
  EXPECT_FALSE(GrantPermission(Map(), origin, "telepathy"));
}

IN_PROC_BROWSER_TEST_F(AurelianPermBrowserTest, ResetToDefault) {
  const std::string origin = "https://reset.example.com";

  // Grant, then reset back to the default ("ask" for geolocation).
  EXPECT_TRUE(GrantPermission(Map(), origin, "geolocation"));
  EXPECT_EQ(QueryPermission(Map(), origin, "geolocation"), "allow");

  EXPECT_TRUE(ResetPermission(Map(), origin, "geolocation"));
  EXPECT_EQ(QueryPermission(Map(), origin, "geolocation"), "ask");

  // Unknown permission cannot be reset.
  EXPECT_FALSE(ResetPermission(Map(), origin, "telepathy"));
}

}  // namespace aurelian
