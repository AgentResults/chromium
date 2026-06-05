// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// AU-WIRE-SERIALIZE (RED-first): the federation-wire reply serializer must
// carry EVERY value kind — bools, doubles, arrays, and nested objects — and
// must JSON-escape string fields. The legacy hand-rolled form dropped the
// richer kinds to "null" and emitted string fields unescaped (malformed /
// injectable JSON on a real wire path: DispatchChromeRoot -> RootDispatch ->
// SerializeWireReply). These assertions fail against that legacy form.

#include "aurelian/handles/root/wire_serialize.h"

#include <memory>
#include <string>

#include "testing/gtest/include/gtest/gtest.h"
#include "velite/agentspaces-wire/handle.hpp"
#include "velite/agentspaces-wire/value_handle.hpp"

namespace aurelian {
namespace {

using velite::agentspaces::Value;
using velite::agentspaces::ValueHandle;

std::string Ser(Value v) {
  return SerializeWireReply(ValueHandle::make(std::move(v)));
}

bool Has(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

// Back-compat: top-level scalars keep their bare form (callers parse them
// directly — e.g. tabs/count -> "3", tabs/activeUrl -> the raw URL).
TEST(WireSerializeTest, TopLevelStringAndIntStayBare) {
  EXPECT_EQ(Ser(Value(std::string("data:text/html,x"))), "data:text/html,x");
  EXPECT_EQ(Ser(Value(int64_t{42})), "42");
}

TEST(WireSerializeTest, TopLevelBoolSerializes) {
  EXPECT_EQ(Ser(Value(true)), "true");
  EXPECT_EQ(Ser(Value(false)), "false");
}

TEST(WireSerializeTest, TopLevelDoubleSerializes) {
  std::string out = Ser(Value(3.5));
  EXPECT_TRUE(Has(out, "3.5")) << out;
  EXPECT_NE(out, "null") << out;
}

TEST(WireSerializeTest, TopLevelArraySerializes) {
  std::string out = Ser(Value::make_array({Value(int64_t{1}), Value(int64_t{2}),
                                           Value(int64_t{3})}));
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
  // {"tabs": [ {"id": 7} ]} — the legacy serializer nulls the nested array.
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
  // A field value with a quote, a backslash, and a newline. The legacy form
  // emitted them raw -> malformed JSON. Proper JSON escapes them.
  std::string out = Ser(Value::make_object(
      {{"r", Value(std::string("a\"b\\c\nd"))}}));
  // Escaped quote: backslash + quote (the 4-char C++ literal "a\\\"b").
  EXPECT_TRUE(Has(out, "a\\\"b")) << "quote not escaped: " << out;
  // Escaped backslash: two backslashes.
  EXPECT_TRUE(Has(out, "\\\\c")) << "backslash not escaped: " << out;
  // Escaped newline: backslash + n (not a raw 0x0A byte).
  EXPECT_TRUE(Has(out, "\\n")) << "newline not escaped: " << out;
  EXPECT_FALSE(Has(out, "\n")) << "raw newline leaked into JSON: " << out;
}

TEST(WireSerializeTest, BrokenAndNullHandles) {
  EXPECT_EQ(SerializeWireReply(ValueHandle::make_broken("out-of-scope")),
            "broken:out-of-scope");
  EXPECT_EQ(SerializeWireReply(nullptr), "broken:null");
}

}  // namespace
}  // namespace aurelian
