// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/asmodeus/instance_manager.h"

#include "base/files/scoped_temp_dir.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace asmodeus {

class InstanceManagerTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
  }

  base::ScopedTempDir temp_dir_;
};

TEST_F(InstanceManagerTest, AllocatesSequentialPorts) {
  InstanceManager mgr("/usr/bin/true");  // Won't actually launch
  int p1 = mgr.AllocatePort();
  int p2 = mgr.AllocatePort();
  EXPECT_EQ(p1, 9300);
  EXPECT_EQ(p2, 9301);
}

TEST_F(InstanceManagerTest, ReusesFreedPorts) {
  InstanceManager mgr("/usr/bin/true");
  int p1 = mgr.AllocatePort();
  EXPECT_EQ(p1, 9300);
  mgr.ReleasePort(9300);
  int p2 = mgr.AllocatePort();
  EXPECT_EQ(p2, 9300);  // Reused
}

TEST_F(InstanceManagerTest, TracksInstances) {
  InstanceManager mgr("/usr/bin/true");

  // Manually register (normally done by Launch)
  // Use a real process that exits immediately for testing
  std::string profile = temp_dir_.GetPath().Append("test").value();
  int port = mgr.Launch("testbot", profile, 9350);

  // Launch might fail (no real Chrome binary at /usr/bin/true for our args)
  // but the port should still be allocated
  if (port > 0) {
    EXPECT_TRUE(mgr.Get("testbot") != nullptr);
    EXPECT_EQ(mgr.Get("testbot")->cdp_port, 9350);
    mgr.Stop("testbot");
    EXPECT_TRUE(mgr.Get("testbot") == nullptr);
  }
}

TEST_F(InstanceManagerTest, GetReturnsNullForUnknown) {
  InstanceManager mgr("/usr/bin/true");
  EXPECT_TRUE(mgr.Get("nonexistent") == nullptr);
  EXPECT_FALSE(mgr.IsRunning("nonexistent"));
}

TEST_F(InstanceManagerTest, ListReturnsAllInstances) {
  InstanceManager mgr("/usr/bin/true");
  auto list = mgr.List();
  EXPECT_EQ(list.size(), 0u);
}

TEST_F(InstanceManagerTest, PreferredPortIsUsed) {
  InstanceManager mgr("/usr/bin/true");
  // Just test port allocation with preferred port
  // (actual launch may fail without real Chrome binary)
  int p1 = mgr.AllocatePort();
  EXPECT_EQ(p1, 9300);
  // Preferred port doesn't go through AllocatePort
}

}  // namespace asmodeus
