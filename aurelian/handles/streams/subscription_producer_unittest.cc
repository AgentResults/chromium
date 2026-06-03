// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C6.a — local subscription loopback (in-process, no Mojo).
//
// RED-first: SubscriptionProducer::Broadcast starts as a no-op stub, so
// LocalLoopbackRoundTrip fails (no frames delivered). Implementing the
// fan-out makes it GREEN. CancelStopsFrames proves a cancelled holder
// stops receiving within one Broadcast.

#include "aurelian/handles/streams/subscription_producer.h"

#include <memory>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"
#include "velite/agentspaces-wire/handle.hpp"
#include "velite/agentspaces-wire/value_handle.hpp"

namespace aurelian {
namespace {

using V = velite::agentspaces::Value;
using Handle = velite::agentspaces::Handle;
using StateKind = velite::agentspaces::StateKind;

// A sink Handle that records every `legion-notify` frame it receives.
class RecordingSink : public Handle {
 public:
  static std::shared_ptr<RecordingSink> make() {
    return std::shared_ptr<RecordingSink>(new RecordingSink());
  }

  StateKind state_kind() const override { return StateKind::ResolvedValue; }
  const V& resolved_value() const override { return value_; }
  std::shared_ptr<Handle> resolved_handle() const override { return nullptr; }
  std::string_view broken_reason() const override { return ""; }

  std::shared_ptr<Handle> ask_impl(std::string_view, const V&) override {
    return velite::agentspaces::ValueHandle::make_broken("not-callable");
  }
  void tell(std::string_view msg, const V& data) override {
    if (msg == "legion-notify") frames_.push_back(data);
  }

  const std::vector<V>& frames() const { return frames_; }

 private:
  RecordingSink() : value_("sink") {}
  V value_;
  std::vector<V> frames_;
};

TEST(AurelianSubscriptionProducerTest, LocalLoopbackRoundTrip) {
  SubscriptionProducer producer;
  auto sink = RecordingSink::make();
  auto sub = producer.Subscribe(sink, "legion://chrome/test", "");
  ASSERT_EQ(producer.active_count(), 1u);

  producer.Broadcast(V("frame-1"));
  ASSERT_EQ(sink->frames().size(), 1u);
  EXPECT_EQ(sink->frames()[0].as_string(), "frame-1");

  producer.Broadcast(V("frame-2"));
  ASSERT_EQ(sink->frames().size(), 2u);
  EXPECT_EQ(sink->frames()[1].as_string(), "frame-2");

  // First delivery promotes the subscription pending -> active.
  auto state = sub->ask("getState", V());
  EXPECT_EQ(state->resolved_value().as_string(), "active");
}

TEST(AurelianSubscriptionProducerTest, CancelStopsFrames) {
  SubscriptionProducer producer;
  auto sink = RecordingSink::make();
  auto sub = producer.Subscribe(sink, "legion://chrome/test", "");

  producer.Broadcast(V("a"));
  EXPECT_EQ(sink->frames().size(), 1u);

  sub->tell("cancel", V());
  producer.Broadcast(V("b"));
  EXPECT_EQ(sink->frames().size(), 1u) << "no frames after cancel";
  EXPECT_EQ(producer.active_count(), 0u);
}

TEST(AurelianSubscriptionProducerTest, MultipleSinksEachReceive) {
  SubscriptionProducer producer;
  auto s1 = RecordingSink::make();
  auto s2 = RecordingSink::make();
  producer.Subscribe(s1, "legion://chrome/test", "");
  producer.Subscribe(s2, "legion://chrome/test", "");

  producer.Broadcast(V("x"));
  EXPECT_EQ(s1->frames().size(), 1u);
  EXPECT_EQ(s2->frames().size(), 1u);
  EXPECT_EQ(producer.active_count(), 2u);
}

}  // namespace
}  // namespace aurelian
