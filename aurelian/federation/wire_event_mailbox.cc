// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/federation/wire_event_mailbox.h"

#include <atomic>
#include <cstdint>
#include <utility>

#include "base/time/time.h"
#include "velite/agentspaces/limits.hpp"
#include "velite/agentspaces-wire/value_handle.hpp"

namespace aurelian {

using velite::agentspaces::Value;
using velite::agentspaces::ValueHandle;

namespace {
// SIZE_MAX = unset; see SetDefaultPerSubscriptionCapForTesting.
std::atomic<size_t> g_default_cap_for_testing{SIZE_MAX};
}  // namespace

WireEventMailbox::WireEventMailbox()
    : per_sub_cap_(velite::agentspaces::limits::kMaxQueueDepthMessages) {
  if (const size_t cap = g_default_cap_for_testing.load(); cap != SIZE_MAX) {
    per_sub_cap_ = cap;
  }
}

// static
void WireEventMailbox::SetDefaultPerSubscriptionCapForTesting(size_t cap) {
  g_default_cap_for_testing.store(cap);
}

WireEventMailbox::~WireEventMailbox() = default;

WireEventMailbox::PushResult WireEventMailbox::Push(const std::string& sub_id,
                                                    Value entry) {
  base::AutoLock hold(lock_);
  if (overflowed_.count(sub_id)) {
    return PushResult::kDropped;
  }
  // CF-8 (design §7): per-delivery re-evaluation of the subscription cap's
  // expiry — the predicate decision is never cached. Past expiry: drop the
  // event, queue the ONE typed terminal, mark the subscription dead.
  if (const auto it = expiry_per_sub_.find(sub_id);
      it != expiry_per_sub_.end() && it->second != 0 &&
      base::Time::Now().ToTimeT() > it->second) {
    overflowed_.insert(sub_id);
    queue_.push_back(Value::make_object(
        {{"sub_id", Value(sub_id)},
         {"msg", Value(std::string("legion-notify"))},
         {"event",
          Value::make_object(
              {{"state", Value(std::string("cancelled"))},
               {"reason",
                Value(std::string("cap-refused:cap-expired"))}})}}));
    return PushResult::kOverflowedNow;
  }
  size_t& queued = queued_per_sub_[sub_id];
  if (queued >= per_sub_cap_) {
    // The bound (design §5.2): terminate typed, never silent loss. ONE
    // terminal cancelled notify rides the queue; the subscription is dead
    // from here (the caller cancels the producer side).
    overflowed_.insert(sub_id);
    queue_.push_back(Value::make_object(
        {{"sub_id", Value(sub_id)},
         {"msg", Value(std::string("legion-notify"))},
         {"event",
          Value::make_object(
              {{"state", Value(std::string("cancelled"))},
               {"reason",
                Value(std::string("legion://errors/SubscriptionOverflow"))}})}}));
    return PushResult::kOverflowedNow;
  }
  ++queued;
  queue_.push_back(std::move(entry));
  return PushResult::kOk;
}

std::vector<Value> WireEventMailbox::DrainAll() {
  base::AutoLock hold(lock_);
  std::vector<Value> out;
  out.reserve(queue_.size());
  while (!queue_.empty()) {
    out.push_back(std::move(queue_.front()));
    queue_.pop_front();
  }
  // Everything queued has been handed to the drain: reset the per-sub
  // counts (the cap bounds UNDRAINED entries).
  queued_per_sub_.clear();
  return out;
}

void WireEventMailbox::SetPerSubscriptionCapForTesting(size_t cap) {
  base::AutoLock hold(lock_);
  per_sub_cap_ = cap;
}

void WireEventMailbox::SetSubscriptionExpiry(const std::string& sub_id,
                                             int64_t expires_unix) {
  base::AutoLock hold(lock_);
  expiry_per_sub_[sub_id] = expires_unix;
}

// --- WireSinkHandle --------------------------------------------------------

// static
std::shared_ptr<WireSinkHandle> WireSinkHandle::make(
    std::string sub_id, WireEventMailbox* mailbox) {
  return std::shared_ptr<WireSinkHandle>(
      new WireSinkHandle(std::move(sub_id), mailbox));
}

WireSinkHandle::WireSinkHandle(std::string sub_id, WireEventMailbox* mailbox)
    : sub_id_(std::move(sub_id)),
      mailbox_(mailbox),
      self_(std::string("legion://chrome/sink/") + sub_id_) {}

velite::agentspaces::StateKind WireSinkHandle::state_kind() const {
  return velite::agentspaces::StateKind::ResolvedValue;
}
const Value& WireSinkHandle::resolved_value() const {
  return self_;
}
std::shared_ptr<velite::agentspaces::Handle> WireSinkHandle::resolved_handle()
    const {
  return nullptr;
}
std::string_view WireSinkHandle::broken_reason() const {
  return "";
}

std::shared_ptr<velite::agentspaces::Handle> WireSinkHandle::ask_impl(
    std::string_view, const Value&) {
  // Sinks are tell-only (kg.md §4.1 — the SubSinkHandle discipline).
  return ValueHandle::make_broken("sink-not-asknable");
}

void WireSinkHandle::tell(std::string_view msg, const Value& data) {
  // UI thread. Plain-data entry by construction: CDP event params are pure
  // protocol JSON; the sub_id relay keeps designation out (design §5.4).
  Value entry = Value::make_object({{"sub_id", Value(sub_id_)},
                                    {"msg", Value(std::string(msg))},
                                    {"event", data}});
  const WireEventMailbox::PushResult res =
      mailbox_->Push(sub_id_, std::move(entry));
  if (res == WireEventMailbox::PushResult::kOverflowedNow && subscription_) {
    // Terminate the producer side too — the mirror SubscriptionHandle's
    // cancel lifecycle (UI thread: the fan-out delivered here on UI).
    subscription_->tell("cancel", Value());
  }
}

void WireSinkHandle::set_subscription(
    std::shared_ptr<velite::agentspaces::Handle> sub) {
  subscription_ = std::move(sub);
}

}  // namespace aurelian
