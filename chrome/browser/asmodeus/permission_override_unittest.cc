// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/asmodeus/permission_override.h"

#include "testing/gtest/include/gtest/gtest.h"

namespace asmodeus {

TEST(PermissionOverrideTest, DefaultDeniesAll) {
  PermissionOverride override;
  EXPECT_FALSE(override.ShouldGrant(Permission::kMicrophone));
  EXPECT_FALSE(override.ShouldGrant(Permission::kCamera));
  EXPECT_FALSE(override.ShouldGrant(Permission::kNotifications));
}

TEST(PermissionOverrideTest, AutoGrantAll) {
  PermissionOverride override;
  override.SetAutoGrant(true);
  EXPECT_TRUE(override.ShouldGrant(Permission::kMicrophone));
  EXPECT_TRUE(override.ShouldGrant(Permission::kCamera));
  EXPECT_TRUE(override.ShouldGrant(Permission::kNotifications));
  EXPECT_TRUE(override.ShouldGrant(Permission::kGeolocation));
  EXPECT_TRUE(override.ShouldGrant(Permission::kClipboard));
}

TEST(PermissionOverrideTest, ExplicitGrantWithoutAutoGrant) {
  PermissionOverride override;
  override.Grant(Permission::kMicrophone);
  EXPECT_TRUE(override.ShouldGrant(Permission::kMicrophone));
  EXPECT_FALSE(override.ShouldGrant(Permission::kCamera));
}

TEST(PermissionOverrideTest, ExplicitDenyOverridesAutoGrant) {
  PermissionOverride override;
  override.SetAutoGrant(true);
  override.Deny(Permission::kGeolocation);
  EXPECT_TRUE(override.ShouldGrant(Permission::kMicrophone));
  EXPECT_FALSE(override.ShouldGrant(Permission::kGeolocation));
}

TEST(PermissionOverrideTest, GrantAllPermission) {
  PermissionOverride override;
  override.Grant(Permission::kAll);
  EXPECT_TRUE(override.ShouldGrant(Permission::kMicrophone));
  EXPECT_TRUE(override.ShouldGrant(Permission::kCamera));
  EXPECT_TRUE(override.ShouldGrant(Permission::kNotifications));
}

TEST(PermissionOverrideTest, DenyAllPermission) {
  PermissionOverride override;
  override.SetAutoGrant(true);
  override.Deny(Permission::kAll);
  EXPECT_FALSE(override.ShouldGrant(Permission::kMicrophone));
  EXPECT_FALSE(override.ShouldGrant(Permission::kCamera));
}

TEST(PermissionOverrideTest, Reset) {
  PermissionOverride override;
  override.SetAutoGrant(true);
  override.Grant(Permission::kMicrophone);
  override.Deny(Permission::kCamera);
  override.Reset();
  EXPECT_FALSE(override.ShouldGrant(Permission::kMicrophone));
  EXPECT_FALSE(override.ShouldGrant(Permission::kCamera));
  EXPECT_FALSE(override.auto_grant());
}

TEST(PermissionOverrideTest, GrantRemovesFromDenied) {
  PermissionOverride override;
  override.Deny(Permission::kMicrophone);
  EXPECT_FALSE(override.ShouldGrant(Permission::kMicrophone));
  override.Grant(Permission::kMicrophone);
  EXPECT_TRUE(override.ShouldGrant(Permission::kMicrophone));
}

TEST(PermissionOverrideTest, ParsePermissionStrings) {
  EXPECT_EQ(ParsePermission("microphone"), Permission::kMicrophone);
  EXPECT_EQ(ParsePermission("mic"), Permission::kMicrophone);
  EXPECT_EQ(ParsePermission("camera"), Permission::kCamera);
  EXPECT_EQ(ParsePermission("cam"), Permission::kCamera);
  EXPECT_EQ(ParsePermission("notifications"), Permission::kNotifications);
  EXPECT_EQ(ParsePermission("clipboard"), Permission::kClipboard);
  EXPECT_EQ(ParsePermission("geolocation"), Permission::kGeolocation);
  EXPECT_EQ(ParsePermission("location"), Permission::kGeolocation);
  EXPECT_EQ(ParsePermission("all"), Permission::kAll);
  EXPECT_EQ(ParsePermission("*"), Permission::kAll);
  EXPECT_EQ(ParsePermission("unknown"), Permission::kAll);
}

TEST(PermissionOverrideTest, PermissionToStringRoundTrip) {
  EXPECT_STREQ(PermissionToString(Permission::kMicrophone), "microphone");
  EXPECT_STREQ(PermissionToString(Permission::kCamera), "camera");
  EXPECT_STREQ(PermissionToString(Permission::kAll), "all");
}

}  // namespace asmodeus
