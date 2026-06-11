// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/capability/cap_gate.h"

#include <vector>

#include "aurelian/capability/cap_membrane.h"
#include "aurelian/capability/cap_wire.h"

namespace aurelian {

namespace {

constexpr char kRefusedPrefix[] = "cap-refused:";

// Federation verbs that only READ projected state. Everything else is
// conservatively classified mutating — the safe direction: under a mode=read
// cap an unknown verb is denied, never silently admitted as a read.
bool IsReadOnlyFederationVerb(const std::string& verb) {
  return verb == "get" || verb == "count" || verb == "activeUrl" ||
         verb == "info" || verb == "list" ||
         verb.compare(0, 5, "__get") == 0;
}

// EnforceCap answers "" or {"error":"<reason>"}; unwrap to the bare reason so
// the wire refusal is one flat typed token.
std::string UnwrapReason(const std::string& enforce_result) {
  constexpr char kPrefix[] = "{\"error\":\"";
  constexpr size_t kPrefixLen = sizeof(kPrefix) - 1;
  if (enforce_result.size() > kPrefixLen + 2 &&
      enforce_result.compare(0, kPrefixLen, kPrefix) == 0) {
    return enforce_result.substr(kPrefixLen,
                                 enforce_result.size() - kPrefixLen - 2);
  }
  return enforce_result;
}

}  // namespace

std::string GateFederationDispatch(const std::string& serialized_chain,
                                   const PubKey& trusted_anchor,
                                   bool has_anchor,
                                   const std::string& uri,
                                   const std::string& verb,
                                   int64_t now_unix) {
  std::vector<CapLink> chain;
  if (!ParseChain(serialized_chain, &chain)) {
    return std::string(kRefusedPrefix) + "cap-chain-malformed";
  }
  const bool is_mutating = !IsReadOnlyFederationVerb(verb);
  std::string denied = EnforceCap(chain, trusted_anchor, has_anchor, verb,
                                  is_mutating, uri, now_unix);
  if (denied.empty()) {
    return std::string();
  }
  std::string reason = UnwrapReason(denied);
  // Subtree canonicalization: a pattern B/* must admit the base node B itself
  // (tested as "B/") and never the sibling id-prefix BX — retry the SAME
  // enforcement with the trailing-slash form before refusing on pattern
  // grounds.
  if (reason == "pattern-mismatch") {
    std::string retry = EnforceCap(chain, trusted_anchor, has_anchor, verb,
                                   is_mutating, uri + "/", now_unix);
    if (retry.empty()) {
      return std::string();
    }
  }
  return std::string(kRefusedPrefix) + reason;
}

}  // namespace aurelian
