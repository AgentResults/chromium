// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/audio/asmodeus/asmodeus_participant_registry.h"

#include <thread>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace asmodeus {

class ParticipantRegistryTest : public testing::Test {
 protected:
  void TearDown() override {
    // Clean up any registrations from the test
    auto& reg = ParticipantRegistry::Get();
    for (const auto& name : registered_names_) {
      reg.UnregisterByName(name);
    }
  }

  void Register(int pid, int fid, const std::string& name) {
    ParticipantRegistry::Get().Register(pid, fid, name);
    registered_names_.push_back(name);
  }

  std::vector<std::string> registered_names_;
};

TEST_F(ParticipantRegistryTest, RegisterAndLookup) {
  Register(100, 1, "alice");

  auto& reg = ParticipantRegistry::Get();
  EXPECT_EQ(reg.Lookup(100, 1), "alice");
  EXPECT_EQ(reg.Lookup(100, 2), "");  // Not registered
  EXPECT_EQ(reg.GetOutputDeviceId(100, 1), "asmodeus-out-alice");
  EXPECT_EQ(reg.GetInputDeviceId(100, 1), "asmodeus-alice");
}

TEST_F(ParticipantRegistryTest, UnregisterByName) {
  Register(100, 1, "bob");
  auto& reg = ParticipantRegistry::Get();
  EXPECT_EQ(reg.Lookup(100, 1), "bob");

  reg.UnregisterByName("bob");
  EXPECT_EQ(reg.Lookup(100, 1), "");
}

TEST_F(ParticipantRegistryTest, MultipleFramesSameParticipant) {
  // Meet uses iframes — multiple frames per participant
  Register(100, 1, "bob");
  Register(100, 2, "bob");
  Register(101, 1, "bob");  // Sub-frame in different process

  auto& reg = ParticipantRegistry::Get();
  EXPECT_EQ(reg.Lookup(100, 1), "bob");
  EXPECT_EQ(reg.Lookup(100, 2), "bob");
  EXPECT_EQ(reg.Lookup(101, 1), "bob");

  reg.UnregisterByName("bob");
  EXPECT_EQ(reg.Lookup(100, 1), "");
  EXPECT_EQ(reg.Lookup(100, 2), "");
  EXPECT_EQ(reg.Lookup(101, 1), "");
}

TEST_F(ParticipantRegistryTest, MultipleParticipants) {
  Register(100, 1, "alice");
  Register(200, 1, "bob");

  auto& reg = ParticipantRegistry::Get();
  EXPECT_EQ(reg.GetOutputDeviceId(100, 1), "asmodeus-out-alice");
  EXPECT_EQ(reg.GetOutputDeviceId(200, 1), "asmodeus-out-bob");
  EXPECT_EQ(reg.GetInputDeviceId(100, 1), "asmodeus-alice");
  EXPECT_EQ(reg.GetInputDeviceId(200, 1), "asmodeus-bob");
}

TEST_F(ParticipantRegistryTest, UnknownFrameReturnsEmpty) {
  auto& reg = ParticipantRegistry::Get();
  EXPECT_EQ(reg.Lookup(999, 999), "");
  EXPECT_EQ(reg.GetOutputDeviceId(999, 999), "");
  EXPECT_EQ(reg.GetInputDeviceId(999, 999), "");
}

TEST_F(ParticipantRegistryTest, ThreadSafety) {
  auto& reg = ParticipantRegistry::Get();
  std::vector<std::thread> threads;

  // Hammer from 10 threads simultaneously
  for (int t = 0; t < 10; t++) {
    threads.emplace_back([&reg, t]() {
      std::string name = "agent" + std::to_string(t);
      for (int i = 0; i < 100; i++) {
        reg.Register(t * 1000 + i, i, name);
        reg.Lookup(t * 1000 + i, i);
        reg.GetOutputDeviceId(t * 1000 + i, i);
        reg.GetInputDeviceId(t * 1000 + i, i);
      }
    });
  }
  for (auto& t : threads)
    t.join();

  // Cleanup
  for (int t = 0; t < 10; t++) {
    reg.UnregisterByName("agent" + std::to_string(t));
  }
}

}  // namespace asmodeus
