// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// HS-1 (ACM-2, AURELIAN-GENERIC-CONTROL-DESIGN section 3): the
// completion-signaled wire bridge. A CDP completion arrives in a LATER UI
// turn than the dispatch, so the wire-side serve thread can no longer run
// the whole dispatch as one synchronous UI task. The mechanism, verbatim
// from the design:
//
//  - The completion record is created and registered ON THE SERVE THREAD,
//    BEFORE posting (round-6): the ONE wait the serve thread ever performs
//    is on this record's event, so every wait is Stop()-coverable by
//    construction — no handshake window.
//  - The record is heap-owned, shared-ownership: {reply slot, WaitableEvent,
//    one-shot outcome in {pending, completed, shutdown}} with the outcome a
//    CAS-advanced atomic — the CAS winner decides. The completion path
//    serializes into the slot FIRST and only then CASes to completed (a
//    reader observing completed has a release/acquire-published slot).
//  - The enumerable live-record set is a SECOND, mutex-protected structure
//    owned by the bridge: the serve thread adds at record creation
//    (pre-post), completion removes (UI thread), Stop() enumerates + signals
//    (any thread). The mutex guards only this set, never Handles or Values.
//  - The settlement registrar (keyed by the pending Handle) is UI-CONFINED;
//    the HS-1 session layer — the code that settles — calls NotifySettled
//    right after settling, which serializes on the UI thread, CASes
//    completed, signals, and ERASES the entry (one-shot).
//  - Timeout: TimedWait expiry replies a typed timeout error and ABANDONS
//    the reference — no unregister round-trip, no wait on the UI thread. A
//    late completion finds the registrar entry, writes the still-alive heap
//    slot, signals an event nobody waits on, and erases — harmless by
//    shared ownership.
//  - Stop() (the shutdown thread, BEFORE the serve threads are joined):
//    CASes every live record to shutdown and signals it without touching
//    the slot; woken waiters reply a typed shutdown error.
//
// ChromeRoot-free and content-free so the mechanism is pinned at unit tier.

#ifndef AURELIAN_FEDERATION_COMPLETION_BRIDGE_H_
#define AURELIAN_FEDERATION_COMPLETION_BRIDGE_H_

#include <atomic>
#include <map>
#include <memory>
#include <set>
#include <string>

#include "aurelian/handles/root/wire_reply.h"
#include "base/no_destructor.h"
#include "base/sequence_checker.h"
#include "base/synchronization/lock.h"
#include "base/synchronization/waitable_event.h"
#include "base/time/time.h"

namespace velite::agentspaces {
class Handle;
}

namespace aurelian {

// The typed refusal REASONS of the two non-completion outcomes. Reasons,
// not replies: WireReply::kind already says these are refusals, so the
// old "broken:" prefix was the kind stuttered into the text.
inline constexpr char kDispatchTimeoutReason[] = "dispatch-timeout";
inline constexpr char kDispatchShutdownReason[] = "dispatch-shutdown";

// The heap-owned, shared-ownership completion record (design section 3).
struct CompletionRecord {
  enum Outcome : int { kPending = 0, kCompleted = 1, kShutdown = 2 };

  // The slot — written by exactly one completion path, published by the
  // winning CAS to kCompleted. Carries its KIND, so a refusal settles the
  // dispatch handle Broken instead of passing for a string answer.
  WireReply reply;
  base::WaitableEvent event;
  std::atomic<int> outcome{kPending};
};

class CompletionBridge {
 public:
  // The process-global bridge both wire bring-ups consume. Unit tests
  // construct their own instances.
  static CompletionBridge& Get();

  CompletionBridge();
  ~CompletionBridge();
  CompletionBridge(const CompletionBridge&) = delete;
  CompletionBridge& operator=(const CompletionBridge&) = delete;

  // SERVE THREAD, BEFORE posting the UI task (round-6: pre-post creation
  // means every serve-thread wait is Stop()-coverable by construction).
  // Adds the record to the live set. After Stop() the record is born
  // shutdown, so the waiter replies typed immediately.
  std::shared_ptr<CompletionRecord> CreateRecord();

  // UI THREAD (the posted dispatch task), already-settled outcome: fill the
  // slot, CAS to completed, signal, remove from the live set. A lost CAS
  // (Stop() won mid-flight) discards the reply and does not signal twice.
  void Complete(const std::shared_ptr<CompletionRecord>& record,
                WireReply reply);

  // UI THREAD, Pending outcome: hand the record to the UI-confined
  // settlement registrar keyed by the pending answer handle; the posted
  // task then RETURNS (the UI thread is free).
  void RegisterPending(const std::shared_ptr<CompletionRecord>& record,
                       std::shared_ptr<velite::agentspaces::Handle> answer);

  // UI THREAD, called by the HS-1 session layer RIGHT AFTER it settles
  // `answer`: serialize the settled answer via SerializeWireReply into the
  // record's slot, CAS to completed, signal, erase the registrar entry
  // (one-shot — stated, not implied). A handle with no registrar entry
  // (in-process dispatch, or an entry already torn down) is a no-op.
  void NotifySettled(const std::shared_ptr<velite::agentspaces::Handle>& answer);

  // SERVE THREAD: the one wait. Returns the published reply, the typed
  // timeout (abandoning the reference — no unregister round-trip), or the
  // typed shutdown error.
  WireReply Wait(const std::shared_ptr<CompletionRecord>& record,
                 base::TimeDelta timeout);

  // SHUTDOWN THREAD, before the serve threads are joined: mark every live
  // record shutdown + signal. The mutex guards only the live set.
  void Stop();

  size_t LiveRecordCountForTesting();
  size_t RegistrarSizeForTesting() const;

 private:
  friend class base::NoDestructor<CompletionBridge>;

  // The mutex-protected live-record set (design section 3, round-5/6).
  base::Lock live_lock_;
  std::set<std::shared_ptr<CompletionRecord>> live_ GUARDED_BY(live_lock_);
  bool stopped_ GUARDED_BY(live_lock_) = false;

  // The UI-confined settlement registrar: pending handle -> (the handle —
  // kept alive — and its record). No lock: every access is on the one UI
  // sequence (checked).
  std::map<const velite::agentspaces::Handle*,
           std::pair<std::shared_ptr<velite::agentspaces::Handle>,
                     std::shared_ptr<CompletionRecord>>>
      registrar_;
  SEQUENCE_CHECKER(ui_sequence_);
};

}  // namespace aurelian

#endif  // AURELIAN_FEDERATION_COMPLETION_BRIDGE_H_
