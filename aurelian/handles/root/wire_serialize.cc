// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/handles/root/wire_serialize.h"

#include <string>

#include "velite/agentspaces-wire/handle.hpp"
#include "velite/agentspaces-wire/json_marshal.hpp"

namespace aurelian {

using velite::agentspaces::Handle;
using velite::agentspaces::StateKind;
using velite::agentspaces::Value;

namespace {

// Whether the shared marshaller can carry this Value losslessly. to_json()
// renders the kinds it cannot represent — an in-process Handle, a wire
// SlotRef, a byte string — as `null`, which at a wire seam is a silent drop
// dressed as an answer. Conversions are lossless or they refuse.
bool CanLower(const Value& v) {
  if (v.is_null() || v.is_bool() || v.is_int() || v.is_double() ||
      v.is_string()) {
    return true;
  }
  if (v.is_array()) {
    for (const Value& item : v.as_array()) {
      if (!CanLower(item)) {
        return false;
      }
    }
    return true;
  }
  if (v.is_object()) {
    for (const auto& [key, val] : v.as_object()) {
      if (!CanLower(val)) {
        return false;
      }
    }
    return true;
  }
  return false;
}

}  // namespace

WireReply SerializeWireValue(const Value& v) {
  if (!CanLower(v)) {
    return WireReply::MakeBroken("conversion-lossy");
  }
  return WireReply::MakeValue(velite::agentspaces::to_json(v).to_string());
}

WireReply SerializeWireReply(const std::shared_ptr<Handle>& h) {
  // ResolvedHandle is the PROMISE shape, not a capability answer: a settled
  // Placeholder holds the real answer in resolved_handle(), while its own
  // resolved_value() is that placeholder URI — a DESIGNATION. Reading the
  // designation as the answer (what this seam used to do, because it never
  // checked the state at all) put a URI on the wire where the caller
  // expected a result. Unwrap instead, bounded, so a cyclic or pathological
  // chain refuses loudly rather than spinning. A live node handle reports
  // ResolvedValue with its identity value and is unaffected.
  constexpr int kMaxUnwrap = 8;
  std::shared_ptr<Handle> cur = h;
  for (int hop = 0; hop <= kMaxUnwrap; ++hop) {
    if (!cur) {
      return WireReply::MakeBroken("null-answer");
    }
    switch (cur->state_kind()) {
      case StateKind::Broken:
        return WireReply::MakeBroken(std::string(cur->broken_reason()));
      case StateKind::Pending:
        // The caller owns the wait; RootDispatch hands Pending answers back
        // rather than serializing them. Arriving here means that contract
        // was broken — reported, never papered over with an empty value.
        return WireReply::MakeBroken("answer-still-pending");
      case StateKind::ResolvedValue:
        return SerializeWireValue(cur->resolved_value());
      case StateKind::ResolvedHandle:
        cur = cur->resolved_handle();
        continue;
    }
  }
  return WireReply::MakeBroken("handle-answer-chain-too-deep");
}

}  // namespace aurelian
