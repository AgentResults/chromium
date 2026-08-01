// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// AU-WIRE-SERIALIZE: the federation-wire reply serializer must carry EVERY
// value kind — bools, doubles, arrays, nested objects — with JSON-escaped
// string fields, and it must carry the reply KIND alongside the payload.
//
// TEST-CHANGE (AU-WIRE-KIND): these assertions previously pinned a bare
// std::string return, in which
//   * a refusal was the string "broken:<reason>", indistinguishable from a
//     successful string answer, and
//   * a top-level scalar was emitted BARE, so the int 3 and the string "3"
//     produced identical bytes and the receiver could not restore the type.
// Both were the old contract and both are gone; the assertions below assert
// the new one. The bare-scalar back-compat test is deleted rather than
// rewritten — the property it protected is the defect.

#include "aurelian/handles/root/wire_serialize.h"

#include <memory>
#include <string>

#include "testing/gtest/include/gtest/gtest.h"
#include "velite/agentspaces-wire/handle.hpp"
#include "velite/agentspaces-wire/placeholder.hpp"
#include "velite/agentspaces-wire/value_handle.hpp"

namespace aurelian {
namespace {

using velite::agentspaces::Placeholder;
using velite::agentspaces::Value;
using velite::agentspaces::ValueHandle;

// The canonical-JSON payload of a value reply. Fails the calling test if the
// reply is a refusal, so a refusal can never masquerade as a payload here
// either.
std::string Ser(Value v) {
  WireReply r = SerializeWireReply(ValueHandle::make(std::move(v)));
  EXPECT_FALSE(r.is_broken()) << "unexpected refusal: " << r.payload;
  return r.payload;
}

bool Has(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

// Scalars are canonical JSON — the type survives the wire.
TEST(WireSerializeTest, TopLevelScalarsKeepTheirType) {
  // A string is QUOTED: the receiver restores a string, and can tell it from
  // the int below, which the old bare form could not.
  EXPECT_EQ(Ser(Value(std::string("data:text/html,x"))), "\"data:text/html,x\"");
  EXPECT_EQ(Ser(Value(int64_t{42})), "42");
  EXPECT_EQ(Ser(Value(true)), "true");
  EXPECT_EQ(Ser(Value(false)), "false");
}

TEST(WireSerializeTest, TopLevelDoubleSerializes) {
  std::string out = Ser(Value(3.5));
  EXPECT_TRUE(Has(out, "3.5")) << out;
  EXPECT_NE(out, "null") << out;
}

TEST(WireSerializeTest, TopLevelArraySerializes) {
  std::string out = Ser(Value::make_array(
      {Value(int64_t{1}), Value(int64_t{2}), Value(int64_t{3})}));
  EXPECT_NE(out, "null") << out;
  EXPECT_EQ(out.front(), '[') << out;
  EXPECT_TRUE(Has(out, "1") && Has(out, "2") && Has(out, "3")) << out;
}

TEST(WireSerializeTest, ObjectBoolFieldNotNulled) {
  // The exact shape tabs/list would carry: a bool `active`.
  std::string out = Ser(Value::make_object({{"active", Value(false)}}));
  EXPECT_TRUE(Has(out, "active")) << out;
  EXPECT_TRUE(Has(out, "false")) << out;
  EXPECT_FALSE(Has(out, "null")) << "bool field collapsed to null: " << out;
}

TEST(WireSerializeTest, NestedArrayAndObjectSurvive) {
  Value inner = Value::make_object({{"id", Value(int64_t{7})}});
  Value arr = Value::make_array({std::move(inner)});
  std::string out = Ser(Value::make_object({{"tabs", std::move(arr)}}));
  EXPECT_TRUE(Has(out, "tabs")) << out;
  EXPECT_TRUE(Has(out, "[")) << "nested array collapsed: " << out;
  EXPECT_TRUE(Has(out, "\"id\"")) << "nested object field lost: " << out;
  EXPECT_TRUE(Has(out, "7")) << out;
  EXPECT_FALSE(Has(out, "null")) << out;
}

TEST(WireSerializeTest, StringFieldsAreJsonEscaped) {
  std::string out =
      Ser(Value::make_object({{"r", Value(std::string("a\"b\\c\nd"))}}));
  EXPECT_TRUE(Has(out, "a\\\"b")) << "quote not escaped: " << out;
  EXPECT_TRUE(Has(out, "\\\\c")) << "backslash not escaped: " << out;
  EXPECT_TRUE(Has(out, "\\n")) << "newline not escaped: " << out;
  EXPECT_FALSE(Has(out, "\n")) << "raw newline leaked into JSON: " << out;
}

// A refusal is a REFUSAL — carried in the kind, with a bare reason.
TEST(WireSerializeTest, BrokenHandleIsARefusalNotAString) {
  WireReply r = SerializeWireReply(ValueHandle::make_broken("out-of-scope"));
  EXPECT_TRUE(r.is_broken());
  EXPECT_EQ(r.payload, "out-of-scope")
      << "the reason must not carry a broken: prefix — the kind says that";

  WireReply n = SerializeWireReply(nullptr);
  EXPECT_TRUE(n.is_broken());
  EXPECT_EQ(n.payload, "null-answer");
}

// A string answer that LOOKS like the old refusal encoding is still a value.
// This is why the kind is carried rather than sniffed back out of the text.
TEST(WireSerializeTest, StringAnswerThatLooksLikeARefusalStaysAValue) {
  WireReply r =
      SerializeWireReply(ValueHandle::make(Value(std::string("broken:nope"))));
  EXPECT_FALSE(r.is_broken());
  EXPECT_EQ(r.payload, "\"broken:nope\"");
}

// A Pending handle must never be serialized as an answer — the caller owns
// the wait. Reaching the serializer is reported, not silently emptied.
TEST(WireSerializeTest, PendingHandleRefuses) {
  WireReply r = SerializeWireReply(Placeholder::make("legion://pending"));
  EXPECT_TRUE(r.is_broken());
  EXPECT_EQ(r.payload, "answer-still-pending");
}

// A settled Placeholder is ResolvedHandle: the ANSWER is the inner handle,
// while its own resolved_value() is the placeholder URI. The seam unwraps to
// the answer instead of emitting the designation.
TEST(WireSerializeTest, SettledPlaceholderYieldsTheInnerAnswer) {
  std::shared_ptr<Placeholder> p = Placeholder::make("legion://answer-slot");
  p->settle(ValueHandle::make(Value(std::string("the-real-answer"))));
  WireReply r = SerializeWireReply(p);
  ASSERT_FALSE(r.is_broken()) << r.payload;
  EXPECT_EQ(r.payload, "\"the-real-answer\"")
      << "the placeholder URI leaked onto the wire in place of the answer";
}

// A Value carrying a kind JSON cannot represent (an in-process handle) must
// refuse. The shared marshaller renders it `null`; emitting that as the
// answer would be a silent drop wearing an answer's clothes.
TEST(WireSerializeTest, NonLowerableValueRefusesRatherThanNulling) {
  Value with_handle = Value::make_object(
      {{"h", Value(ValueHandle::make(Value(int64_t{1})))}});
  WireReply r = SerializeWireReply(ValueHandle::make(std::move(with_handle)));
  EXPECT_TRUE(r.is_broken()) << "silently lowered to: " << r.payload;
  EXPECT_EQ(r.payload, "conversion-lossy");
}

}  // namespace
}  // namespace aurelian
