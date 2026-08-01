// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// CF-6 (unit): PendingDispatchHandle maps the CompletionRecord's CAS states
// onto the substrate Handle lifecycle (design §6.1) — kPending → Pending,
// kShutdown → Broken(kDispatchShutdownReason); a deadline maps expiry to
// Broken(kDispatchTimeoutReason), checked in state_kind() so the timeout
// needs no timer thread.
//
// AU-WIRE-KIND (RED-first): a completed record carries its KIND, and the
// dispatch handle settles ACCORDINGLY. Two defects this pins:
//
//   1. A refusal MUST settle Broken. The legacy path collapsed every
//      completed record to ResolvedValue holding the string
//      "broken:<reason>" — a refusal wearing success's clothes at the
//      embodiment's wire seam, so a caller checking the envelope kind
//      read success and went on to parse a plausible string. That is
//      "nothing ever silently no-ops" violated at the one seam where the
//      whole browser embodiment answers the federation.
//   2. A structured answer MUST settle as the TYPED Value. The legacy path
//      handed back canonical JSON as a *string*, so an object arrived
//      indistinguishable from a string that happens to look like JSON, and
//      every consumer had to re-parse to recover the type.

#include "aurelian/federation/pending_dispatch.h"

#include <memory>

#include "aurelian/federation/completion_bridge.h"
#include "base/time/time.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "velite/agentspaces-wire/handle.hpp"
#include "aurelian/handles/root/wire_serialize.h"
#include "velite/agentspaces-wire/value_handle.hpp"

namespace aurelian {
namespace {

using velite::agentspaces::StateKind;
using velite::agentspaces::Value;
using velite::agentspaces::ValueHandle;

// Settles `answer` through the bridge into a fresh dispatch handle — the
// exact production path (RegisterPending on the UI turn, NotifySettled when
// the session layer settles).
std::shared_ptr<PendingDispatchHandle> SettleThroughBridge(
    CompletionBridge& bridge,
    const std::shared_ptr<velite::agentspaces::Handle>& answer) {
  auto record = bridge.CreateRecord();
  auto handle = PendingDispatchHandle::make(record, base::TimeTicks());
  bridge.RegisterPending(record, answer);
  bridge.NotifySettled(answer);
  return handle;
}

TEST(PendingDispatchHandleTest, MapsRecordStates) {
  auto record = std::make_shared<CompletionRecord>();
  auto handle = PendingDispatchHandle::make(record, base::TimeTicks());

  // kPending → Pending (no deadline set).
  EXPECT_EQ(handle->state_kind(), StateKind::Pending);

  // kShutdown → Broken with the typed shutdown reason.
  auto record2 = std::make_shared<CompletionRecord>();
  auto handle2 = PendingDispatchHandle::make(record2, base::TimeTicks());
  record2->outcome.store(CompletionRecord::kShutdown);
  EXPECT_EQ(handle2->state_kind(), StateKind::Broken);
  EXPECT_EQ(handle2->broken_reason(), kDispatchShutdownReason);
}

TEST(PendingDispatchHandleTest, DeadlineBreaksTyped) {
  auto record = std::make_shared<CompletionRecord>();
  // A deadline already in the past: the pending record reads Broken with
  // the typed timeout — no timer thread, the check lives in state_kind().
  auto handle = PendingDispatchHandle::make(
      record, base::TimeTicks::Now() - base::Seconds(1));
  EXPECT_EQ(handle->state_kind(), StateKind::Broken);
  EXPECT_EQ(handle->broken_reason(), kDispatchTimeoutReason);

  // A completion that already won the CAS outranks the deadline (the
  // settled answer is real; expiry only covers the never-settling case).
  CompletionBridge bridge;
  auto record2 = bridge.CreateRecord();
  auto handle2 = PendingDispatchHandle::make(
      record2, base::TimeTicks::Now() - base::Seconds(1));
  auto answer = ValueHandle::make(Value(std::string("late-but-settled")));
  bridge.RegisterPending(record2, answer);
  bridge.NotifySettled(answer);
  EXPECT_EQ(handle2->state_kind(), StateKind::ResolvedValue);
  ASSERT_TRUE(handle2->resolved_value().is_string());
  EXPECT_EQ(handle2->resolved_value().as_string(), "late-but-settled");
}

// AU-WIRE-KIND 1 — the refusal must be a REFUSAL.
TEST(PendingDispatchHandleTest, RefusalSettlesBrokenNotAStringValue) {
  CompletionBridge bridge;
  auto handle =
      SettleThroughBridge(bridge, ValueHandle::make_broken("out-of-scope"));

  EXPECT_EQ(handle->state_kind(), StateKind::Broken)
      << "a capability refusal settled as a VALUE — success and refusal are "
         "indistinguishable at the wire seam";
  EXPECT_EQ(handle->broken_reason(), "out-of-scope")
      << "the reason must be the reason, not a broken:-prefixed blob";
}

// A null answer handle is equally a refusal, not a string "broken:null".
TEST(PendingDispatchHandleTest, NullAnswerSettlesBroken) {
  CompletionBridge bridge;
  auto record = bridge.CreateRecord();
  auto handle = PendingDispatchHandle::make(record, base::TimeTicks());
  bridge.Complete(record, SerializeWireReply(nullptr));
  EXPECT_EQ(handle->state_kind(), StateKind::Broken);
  EXPECT_EQ(handle->broken_reason(), "null-answer");
}

// A value reply that is not canonical JSON is a wire DEFECT, and surfaces as
// a refusal rather than as a plausible-looking string answer.
TEST(PendingDispatchHandleTest, UnparseableValuePayloadSettlesBroken) {
  CompletionBridge bridge;
  auto record = bridge.CreateRecord();
  auto handle = PendingDispatchHandle::make(record, base::TimeTicks());
  bridge.Complete(record, WireReply::MakeValue("{not json"));
  EXPECT_EQ(handle->state_kind(), StateKind::Broken);
  EXPECT_EQ(handle->broken_reason(), "wire-reply-unparseable");
}

// AU-WIRE-KIND 2 — structure survives as STRUCTURE.
TEST(PendingDispatchHandleTest, StructuredAnswerSettlesTyped) {
  CompletionBridge bridge;
  // The exact shape tabs/list carries: an array of objects with a bool.
  Value tab = Value::make_object({{"id", Value(int64_t{7})},
                                  {"active", Value(false)},
                                  {"url", Value(std::string("https://x/"))}});
  Value answer_value =
      Value::make_object({{"tabs", Value::make_array({std::move(tab)})}});
  auto handle = SettleThroughBridge(
      bridge, ValueHandle::make(std::move(answer_value)));

  ASSERT_EQ(handle->state_kind(), StateKind::ResolvedValue);
  const Value& v = handle->resolved_value();
  ASSERT_TRUE(v.is_object())
      << "the answer flattened to a JSON string — the caller must re-parse "
         "to recover a type the wire already knew";
  const Value* tabs = v.object_get("tabs");
  ASSERT_TRUE(tabs && tabs->is_array());
  ASSERT_EQ(tabs->as_array().size(), 1u);
  const Value& t0 = tabs->as_array()[0];
  ASSERT_TRUE(t0.is_object());
  const Value* id = t0.object_get("id");
  ASSERT_TRUE(id && id->is_int());
  EXPECT_EQ(id->as_int(), 7);
  const Value* active = t0.object_get("active");
  ASSERT_TRUE(active && active->is_bool()) << "bool collapsed";
  EXPECT_FALSE(active->as_bool());
}

// A scalar keeps its TYPE too — tabs/count is the int 3, never the string "3".
TEST(PendingDispatchHandleTest, ScalarAnswerKeepsItsType) {
  CompletionBridge bridge;
  auto ints = SettleThroughBridge(bridge, ValueHandle::make(Value(int64_t{3})));
  ASSERT_EQ(ints->state_kind(), StateKind::ResolvedValue);
  ASSERT_TRUE(ints->resolved_value().is_int())
      << "an int answer arrived as a string";
  EXPECT_EQ(ints->resolved_value().as_int(), 3);

  auto bools = SettleThroughBridge(bridge, ValueHandle::make(Value(true)));
  ASSERT_TRUE(bools->resolved_value().is_bool());
  EXPECT_TRUE(bools->resolved_value().as_bool());

  // And a string answer stays a string — including one that LOOKS like a
  // refusal. Prefix-sniffing a bare reply string would misread this as
  // broken; the kind is carried, never inferred.
  auto strs = SettleThroughBridge(
      bridge, ValueHandle::make(Value(std::string("broken:not-really"))));
  ASSERT_EQ(strs->state_kind(), StateKind::ResolvedValue)
      << "a string answer was prefix-sniffed into a refusal";
  ASSERT_TRUE(strs->resolved_value().is_string());
  EXPECT_EQ(strs->resolved_value().as_string(), "broken:not-really");
}

}  // namespace
}  // namespace aurelian
