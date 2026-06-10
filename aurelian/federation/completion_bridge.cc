// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/federation/completion_bridge.h"

#include <utility>

#include "aurelian/handles/root/wire_serialize.h"
#include "base/check.h"
#include "velite/agentspaces-wire/handle.hpp"

namespace aurelian {

using velite::agentspaces::Handle;
using velite::agentspaces::StateKind;

CompletionBridge& CompletionBridge::Get() {
  static base::NoDestructor<CompletionBridge> bridge;
  return *bridge;
}

CompletionBridge::CompletionBridge() {
  // The registrar's sequence is whichever ("UI") sequence touches it first.
  DETACH_FROM_SEQUENCE(ui_sequence_);
}

CompletionBridge::~CompletionBridge() = default;

std::shared_ptr<CompletionRecord> CompletionBridge::CreateRecord() {
  auto record = std::make_shared<CompletionRecord>();
  base::AutoLock lock(live_lock_);
  if (stopped_) {
    // Born shutdown: the serve thread that raced past Stop() replies typed
    // immediately instead of waiting out a timeout nobody will ever signal.
    record->outcome.store(CompletionRecord::kShutdown,
                          std::memory_order_release);
    record->event.Signal();
    return record;
  }
  live_.insert(record);
  return record;
}

void CompletionBridge::Complete(
    const std::shared_ptr<CompletionRecord>& record,
    std::string reply) {
  // Slot FIRST, then the CAS publishes it (release; the waiter's acquire
  // load pairs with it). A lost CAS (Stop() won) discards the reply — the
  // waiter was already signalled once, never twice.
  record->reply = std::move(reply);
  int expected = CompletionRecord::kPending;
  if (record->outcome.compare_exchange_strong(
          expected, CompletionRecord::kCompleted, std::memory_order_release,
          std::memory_order_relaxed)) {
    record->event.Signal();
  }
  base::AutoLock lock(live_lock_);
  live_.erase(record);
}

void CompletionBridge::RegisterPending(
    const std::shared_ptr<CompletionRecord>& record,
    std::shared_ptr<Handle> answer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(ui_sequence_);
  const Handle* key = answer.get();
  registrar_.emplace(key, std::make_pair(std::move(answer), record));
}

void CompletionBridge::NotifySettled(const std::shared_ptr<Handle>& answer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(ui_sequence_);
  auto it = registrar_.find(answer.get());
  if (it == registrar_.end()) {
    // In-process dispatch (no wire waiter) or an already-erased entry —
    // one-shot by construction.
    return;
  }
  std::shared_ptr<CompletionRecord> record = std::move(it->second.second);
  registrar_.erase(it);
  // The session layer settles BEFORE notifying; serialization never sees a
  // Pending handle (the four-state contract, design section 3).
  DCHECK(answer->state_kind() != StateKind::Pending);
  Complete(record, SerializeWireReply(answer));
}

std::string CompletionBridge::Wait(
    const std::shared_ptr<CompletionRecord>& record,
    base::TimeDelta timeout) {
  record->event.TimedWait(timeout);
  // Read the outcome whether woken or expired — a completion that raced the
  // expiry still counts (its reply is CAS-published).
  switch (record->outcome.load(std::memory_order_acquire)) {
    case CompletionRecord::kCompleted:
      return record->reply;
    case CompletionRecord::kShutdown:
      return kDispatchShutdownReply;
    default:
      // TimedWait expiry: typed timeout, and the reference is simply
      // ABANDONED — no unregister round-trip, no wait on the UI thread. A
      // late completion writes the still-alive heap slot, signals an event
      // nobody waits on, and erases — harmless by shared ownership.
      return kDispatchTimeoutReply;
  }
}

void CompletionBridge::Stop() {
  // The shutdown thread (NOT the serve thread, not necessarily the UI
  // thread): enumerate + CAS + signal under the live-set mutex only — never
  // a Handle, a Value, or the UI-confined registrar.
  base::AutoLock lock(live_lock_);
  stopped_ = true;
  for (const std::shared_ptr<CompletionRecord>& record : live_) {
    int expected = CompletionRecord::kPending;
    if (record->outcome.compare_exchange_strong(
            expected, CompletionRecord::kShutdown, std::memory_order_release,
            std::memory_order_relaxed)) {
      record->event.Signal();
    }
  }
  live_.clear();
}

size_t CompletionBridge::LiveRecordCountForTesting() {
  base::AutoLock lock(live_lock_);
  return live_.size();
}

size_t CompletionBridge::RegistrarSizeForTesting() const {
  return registrar_.size();
}

}  // namespace aurelian
