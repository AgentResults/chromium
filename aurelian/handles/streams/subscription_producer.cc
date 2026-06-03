// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/handles/streams/subscription_producer.h"

#include "base/strings/string_number_conversions.h"
#include "velite/agentspaces-wire/subscription_handle.hpp"
#include "velite/agentspaces-wire/value_handle.hpp"

namespace aurelian {

using Handle = velite::agentspaces::Handle;
using SubscriptionHandle = velite::agentspaces::SubscriptionHandle;
using Value = velite::agentspaces::Value;

SubscriptionProducer::SubscriptionProducer() = default;
SubscriptionProducer::~SubscriptionProducer() = default;

std::shared_ptr<Handle> SubscriptionProducer::Subscribe(
    std::shared_ptr<Handle> sink,
    const std::string& uri_prefix,
    const std::string& filter) {
  std::string id =
      uri_prefix + "/subscription/" + base::NumberToString(next_id_++);
  auto sub = SubscriptionHandle::make(id, std::move(sink), filter);
  subs_.push_back(sub);
  return sub;
}

void SubscriptionProducer::Broadcast(const Value& frame) {
  std::vector<std::shared_ptr<SubscriptionHandle>> live;
  live.reserve(subs_.size());
  for (auto& sub : subs_) {
    if (!sub->active()) {
      continue;  // holder cancelled (or KG failed it) -> drop
    }
    sub->mark_active();  // pending -> active on first delivery
    if (const auto& sink = sub->sink()) {
      sink->tell("legion-notify", frame);
    }
    live.push_back(std::move(sub));
  }
  subs_.swap(live);
}

size_t SubscriptionProducer::active_count() const {
  size_t n = 0;
  for (const auto& sub : subs_) {
    if (sub->active()) ++n;
  }
  return n;
}

}  // namespace aurelian
