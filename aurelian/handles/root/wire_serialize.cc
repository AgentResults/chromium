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

std::string SerializeWireReply(const std::shared_ptr<Handle>& h) {
  if (!h) {
    return "broken:null";
  }
  if (h->state_kind() == StateKind::Broken) {
    return std::string("broken:") + std::string(h->broken_reason());
  }
  const Value& v = h->resolved_value();
  // Top-level scalars keep their BARE form — the wire's "verb -> scalar"
  // contract that callers parse directly (e.g. tabs/count -> "3",
  // tabs/activeUrl -> the raw URL). Quoting them would break those callers.
  if (v.is_string()) {
    return v.as_string();
  }
  if (v.is_int()) {
    return std::to_string(v.as_int());
  }
  // Every richer kind — bools, doubles, arrays, objects, and arbitrary nesting
  // — goes through the shared Velite marshaller, which renders canonical JSON
  // with correct string escaping. No kind is silently dropped to "null", and
  // string fields can no longer emit malformed/injectable JSON.
  return velite::agentspaces::to_json(v).to_string();
}

}  // namespace aurelian
