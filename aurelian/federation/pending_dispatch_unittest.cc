// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// CF-6 RED (unit): PendingDispatchHandle maps the UNCHANGED
// CompletionRecord's CAS states onto the substrate Handle lifecycle
// (design §6.1) — kPending → Pending, kCompleted → ResolvedValue (the
// published reply), kShutdown → Broken(kDispatchShutdownReply); a deadline
// maps expiry to Broken(kDispatchTimeoutReply), checked in state_kind() so
// the timeout needs no timer thread.

#include "aurelian/federation/pending_dispatch.h"

#include <memory>

#include "aurelian/federation/completion_bridge.h"
#include "base/time/time.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "velite/agentspaces-wire/handle.hpp"

namespace aurelian {
namespace {

using velite::agentspaces::StateKind;

TEST(PendingDispatchHandleTest, MapsRecordStates) {
  auto record = std::make_shared<CompletionRecord>();
  auto handle = PendingDispatchHandle::make(record, base::TimeTicks());

  // kPending → Pending (no deadline set).
  EXPECT_EQ(handle->state_kind(), StateKind::Pending);

  // The completion path's discipline: publish the slot, THEN CAS.
  record->reply = "the-published-reply";
  record->outcome.store(CompletionRecord::kCompleted);
  EXPECT_EQ(handle->state_kind(), StateKind::ResolvedValue);
  ASSERT_TRUE(handle->resolved_value().is_string());
  EXPECT_EQ(handle->resolved_value().as_string(), "the-published-reply");

  // kShutdown → Broken with the typed shutdown reply.
  auto record2 = std::make_shared<CompletionRecord>();
  auto handle2 = PendingDispatchHandle::make(record2, base::TimeTicks());
  record2->outcome.store(CompletionRecord::kShutdown);
  EXPECT_EQ(handle2->state_kind(), StateKind::Broken);
  EXPECT_EQ(handle2->broken_reason(), kDispatchShutdownReply);
}

TEST(PendingDispatchHandleTest, DeadlineBreaksTyped) {
  auto record = std::make_shared<CompletionRecord>();
  // A deadline already in the past: the pending record reads Broken with
  // the typed timeout — no timer thread, the check lives in state_kind().
  auto handle = PendingDispatchHandle::make(
      record, base::TimeTicks::Now() - base::Seconds(1));
  EXPECT_EQ(handle->state_kind(), StateKind::Broken);
  EXPECT_EQ(handle->broken_reason(), kDispatchTimeoutReply);

  // A completion that already won the CAS outranks the deadline (the
  // settled answer is real; expiry only covers the never-settling case).
  auto record2 = std::make_shared<CompletionRecord>();
  record2->reply = "late-but-settled";
  record2->outcome.store(CompletionRecord::kCompleted);
  auto handle2 = PendingDispatchHandle::make(
      record2, base::TimeTicks::Now() - base::Seconds(1));
  EXPECT_EQ(handle2->state_kind(), StateKind::ResolvedValue);
  EXPECT_EQ(handle2->resolved_value().as_string(), "late-but-settled");
}

}  // namespace
}  // namespace aurelian
