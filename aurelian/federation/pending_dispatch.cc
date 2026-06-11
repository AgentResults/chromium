// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/federation/pending_dispatch.h"

#include <utility>

#include "aurelian/federation/completion_bridge.h"
#include "velite/agentspaces-wire/value_handle.hpp"

namespace aurelian {

using velite::agentspaces::StateKind;
using velite::agentspaces::Value;
using velite::agentspaces::ValueHandle;

// static
std::shared_ptr<PendingDispatchHandle> PendingDispatchHandle::make(
    std::shared_ptr<CompletionRecord> record, base::TimeTicks deadline) {
  return std::shared_ptr<PendingDispatchHandle>(
      new PendingDispatchHandle(std::move(record), deadline));
}

PendingDispatchHandle::PendingDispatchHandle(
    std::shared_ptr<CompletionRecord> record, base::TimeTicks deadline)
    : record_(std::move(record)), deadline_(deadline) {}

StateKind PendingDispatchHandle::state_kind() const {
  // acquire pairs with the completion path's slot-then-CAS publish.
  const int outcome = record_->outcome.load(std::memory_order_acquire);
  if (outcome == CompletionRecord::kCompleted) {
    return StateKind::ResolvedValue;
  }
  if (outcome == CompletionRecord::kShutdown) {
    broken_reason_ = kDispatchShutdownReply;
    return StateKind::Broken;
  }
  if (!deadline_.is_null() && base::TimeTicks::Now() >= deadline_) {
    broken_reason_ = kDispatchTimeoutReply;
    return StateKind::Broken;
  }
  return StateKind::Pending;
}

const Value& PendingDispatchHandle::resolved_value() const {
  if (!value_cached_ &&
      record_->outcome.load(std::memory_order_acquire) ==
          CompletionRecord::kCompleted) {
    value_ = Value(record_->reply);
    value_cached_ = true;
  }
  return value_;
}

std::shared_ptr<velite::agentspaces::Handle>
PendingDispatchHandle::resolved_handle() const {
  return nullptr;
}

std::string_view PendingDispatchHandle::broken_reason() const {
  return broken_reason_;
}

std::shared_ptr<velite::agentspaces::Handle> PendingDispatchHandle::ask_impl(
    std::string_view, const Value&) {
  // Asks against a settled answer dispatch per [HANDLE-RESOLVED-VALUE-
  // DISPATCH] at the DISPATCHER layer (the pipelining drain); the handle
  // itself exposes no method surface.
  return ValueHandle::make_broken("not-callable");
}

void PendingDispatchHandle::tell(std::string_view, const Value&) {}

}  // namespace aurelian
