// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/capability/cap_membrane.h"

#include "aurelian/capability/cap_predicate.h"

namespace aurelian {

std::string EnforceCap(const std::vector<CapLink>& chain,
                       const PubKey& trusted_anchor,
                       bool has_anchor,
                       const std::string& verb,
                       bool is_mutating,
                       const std::string& target,
                       int64_t now_unix) {
  // No anchor provisioned → this membrane trusts nothing (it cannot vouch for
  // itself).
  if (!has_anchor) {
    return "{\"error\":\"cap-untrusted-anchor\"}";
  }

  // 1. Verify the WHOLE chain back to the trusted anchor.
  ChainVerifyResult result =
      VerifyChain(chain, trusted_anchor, now_unix, /*revoked=*/{});
  if (!result.ok) {
    return "{\"error\":\"" + result.reason + "\"}";
  }

  // 2. Enforce the effective (most-attenuated) predicate on this op.
  std::string reason;
  if (!result.effective.Allows(verb, is_mutating, target, now_unix, &reason)) {
    return "{\"error\":\"" + reason + "\"}";
  }
  return std::string();
}

}  // namespace aurelian
