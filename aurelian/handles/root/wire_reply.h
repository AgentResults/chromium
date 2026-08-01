// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian: the federation wire's reply, WITH ITS KIND.
//
// The kind is CARRIED, never inferred. A refusal that travelled as the bare
// string "broken:<reason>" is indistinguishable from a successful string
// answer: the caller sees a value envelope, reads a plausible string, and a
// capability refusal silently passes for success at the one seam where the
// browser embodiment answers the federation. Recovering the kind by sniffing
// the prefix back out is the same bug wearing a fix's clothes — a legitimate
// answer whose text happens to begin "broken:" would be misread as a refusal.
//
// Velite-free ON PURPOSE, so root_handle.h and completion_bridge.h can carry
// a WireReply without leaking Velite types into Chrome/test TUs (the C-API
// discipline those headers already keep).

#ifndef AURELIAN_HANDLES_ROOT_WIRE_REPLY_H_
#define AURELIAN_HANDLES_ROOT_WIRE_REPLY_H_

#include <ostream>
#include <string>
#include <utility>

namespace aurelian {

struct WireReply {
  enum class Kind { kValue, kBroken };

  Kind kind = Kind::kValue;

  // kValue:  the answer as canonical JSON — EVERY kind, scalars included.
  //          A bare scalar would lose its type (the int 3 and the string "3"
  //          are the same three bytes), leaving the receiver unable to restore
  //          the Value the wire already knew.
  // kBroken: the refusal reason, with NO "broken:" prefix. `kind` already says
  //          it is a refusal; repeating that in the text is how the prefix
  //          leaked into reasons in the first place.
  std::string payload;

  static WireReply MakeValue(std::string canonical_json) {
    return WireReply{Kind::kValue, std::move(canonical_json)};
  }
  static WireReply MakeBroken(std::string reason) {
    return WireReply{Kind::kBroken, std::move(reason)};
  }

  bool is_broken() const { return kind == Kind::kBroken; }
};

// Test/log rendering. Prints the KIND alongside the payload, so a failure
// message can never show a refusal as though it were a value — the same
// confusion the kind field exists to end.
inline std::ostream& operator<<(std::ostream& os, const WireReply& r) {
  return os << (r.is_broken() ? "broken(" : "value(") << r.payload << ")";
}

}  // namespace aurelian

#endif  // AURELIAN_HANDLES_ROOT_WIRE_REPLY_H_
