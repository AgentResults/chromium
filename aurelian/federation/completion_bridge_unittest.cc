// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// ACM-2 unit pins (AURELIAN-GENERIC-CONTROL-TDD-PLAN): the
// completion-signaled bridge mechanism, VERBATIM with design section 3
// (round-5 review M3/M4, round-6 M3) — TimedWait expiry abandons the
// reference (no unregister round-trip, no wait on the UI thread); a late
// settlement finds the registrar entry, writes the still-alive heap-owned
// record, signals the unwaited event, and erases (ONE-SHOT); Stop() CASes
// every live record to shutdown and signals it so no serve thread is left
// blocked once the UI loop stops pumping; the CAS winner decides.
//
// "UI thread" here is the test main thread (the registrar is sequence-
// confined, not content-bound — that is what makes the unit tier honest);
// serve threads are real threads.

#include "aurelian/federation/completion_bridge.h"

#include <memory>
#include <string>
#include <thread>

#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "velite/agentspaces-wire/placeholder.hpp"
#include "velite/agentspaces-wire/value_handle.hpp"

namespace aurelian {

namespace {

using velite::agentspaces::Placeholder;
using velite::agentspaces::Value;
using velite::agentspaces::ValueHandle;

TEST(CompletionBridgeTest, CompletedRecordPublishesReply) {
  CompletionBridge bridge;
  std::shared_ptr<CompletionRecord> record = bridge.CreateRecord();
  EXPECT_EQ(bridge.LiveRecordCountForTesting(), 1u);

  bridge.Complete(record, "the-reply");
  EXPECT_EQ(bridge.Wait(record, base::Seconds(5)), "the-reply");
  // Completion removed the record from the live set (design section 3:
  // serve thread adds, completion removes, Stop() enumerates).
  EXPECT_EQ(bridge.LiveRecordCountForTesting(), 0u);
}

TEST(CompletionBridgeTest, WaiterWokenByCompletionFromAnotherThread) {
  CompletionBridge bridge;
  std::shared_ptr<CompletionRecord> record = bridge.CreateRecord();

  std::thread completer([&bridge, record]() {
    base::PlatformThread::Sleep(base::Milliseconds(50));
    bridge.Complete(record, "threaded-reply");
  });
  EXPECT_EQ(bridge.Wait(record, base::Seconds(5)), "threaded-reply");
  completer.join();
}

// The timeout pin, verbatim with design section 3: TimedWait expiry ->
// typed timeout, the reference simply ABANDONED; the late settlement finds
// the registrar entry, writes the still-alive heap slot, signals the
// unwaited event, and erases — one-shot, harmless by shared ownership.
TEST(CompletionBridgeTest, TimedWaitAbandonsThenLateSettlementErasesOnce) {
  CompletionBridge bridge;
  std::shared_ptr<CompletionRecord> record = bridge.CreateRecord();

  // The pending answer handle (the vendored Pending-state handle; settling
  // it resolves to its determined URI, which SerializeWireReply emits).
  std::shared_ptr<Placeholder> answer = Placeholder::make("late-answer-uri");
  bridge.RegisterPending(record, answer);
  EXPECT_EQ(bridge.RegistrarSizeForTesting(), 1u);

  // Nobody settles: the serve-thread wait expires typed.
  EXPECT_EQ(bridge.Wait(record, base::Milliseconds(1)),
            kDispatchTimeoutReply);

  // The LATE settlement: the entry is still there (no unregister round-trip
  // happened), the heap record is still alive (shared ownership), the write
  // lands, the unwaited event is signalled, and the entry is erased.
  answer->settle(ValueHandle::make(Value(std::string("unused"))));
  bridge.NotifySettled(answer);
  EXPECT_EQ(record->outcome.load(), CompletionRecord::kCompleted);
  EXPECT_EQ(record->reply, "late-answer-uri");
  EXPECT_TRUE(record->event.IsSignaled());
  EXPECT_EQ(bridge.RegistrarSizeForTesting(), 0u);
  EXPECT_EQ(bridge.LiveRecordCountForTesting(), 0u);

  // ONE-SHOT: a second delivery finds nothing and changes nothing.
  bridge.NotifySettled(answer);
  EXPECT_EQ(record->reply, "late-answer-uri");
  EXPECT_EQ(bridge.RegistrarSizeForTesting(), 0u);
}

// Stop() drain (review N3's seam): a serve thread blocked on a record whose
// completion can never arrive is woken BEFORE join with the typed shutdown
// reply — Stop() runs on the shutdown thread, touches only the mutex-
// protected live set, and never waits on anything.
TEST(CompletionBridgeTest, StopWakesBlockedWaiterTyped) {
  CompletionBridge bridge;
  std::shared_ptr<CompletionRecord> record = bridge.CreateRecord();

  std::string result;
  std::thread waiter([&bridge, &record, &result]() {
    result = bridge.Wait(record, base::Seconds(30));
  });
  // Let the waiter block, then drain. The join below is the deadlock probe:
  // pre-HS-1, a post-and-wait with a stopped UI loop blocked it forever.
  base::PlatformThread::Sleep(base::Milliseconds(50));
  bridge.Stop();
  waiter.join();
  EXPECT_EQ(result, kDispatchShutdownReply);
}

// The CAS winner decides (design section 3, round-5 M4): a completion that
// loses to Stop() discards its serialization and does not signal twice; a
// completion that already won keeps its reply through Stop().
TEST(CompletionBridgeTest, CasOneShotOutcome) {
  CompletionBridge bridge;

  // Stop() first -> the completion loses.
  std::shared_ptr<CompletionRecord> lost = bridge.CreateRecord();
  bridge.Stop();
  bridge.Complete(lost, "too-late");
  EXPECT_EQ(lost->outcome.load(), CompletionRecord::kShutdown);
  EXPECT_EQ(bridge.Wait(lost, base::Seconds(5)), kDispatchShutdownReply);
}

TEST(CompletionBridgeTest, CompletionBeforeStopKeepsReply) {
  CompletionBridge bridge;
  std::shared_ptr<CompletionRecord> record = bridge.CreateRecord();
  bridge.Complete(record, "kept");
  bridge.Stop();
  EXPECT_EQ(record->outcome.load(), CompletionRecord::kCompleted);
  EXPECT_EQ(bridge.Wait(record, base::Seconds(5)), "kept");
}

// After Stop() a record is born shutdown: the serve thread that raced past
// the stop flag replies typed immediately instead of waiting out a timeout
// against a UI loop that will never pump.
TEST(CompletionBridgeTest, RecordsBornShutdownAfterStop) {
  CompletionBridge bridge;
  bridge.Stop();
  std::shared_ptr<CompletionRecord> record = bridge.CreateRecord();
  base::TimeTicks start = base::TimeTicks::Now();
  EXPECT_EQ(bridge.Wait(record, base::Seconds(30)), kDispatchShutdownReply);
  EXPECT_LT(base::TimeTicks::Now() - start, base::Seconds(5));
}

}  // namespace

}  // namespace aurelian
