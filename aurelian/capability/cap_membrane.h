// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C8.e: the membrane cap check, performed identically in EVERY
// membrane (renderer DOM/JS, browser/Chromium domain, ...): verify the signed
// chain back to the trusted anchor, then enforce the effective predicate on the
// op. One implementation so attenuation + chain-of-trust hold uniformly.

#ifndef AURELIAN_CAPABILITY_CAP_MEMBRANE_H_
#define AURELIAN_CAPABILITY_CAP_MEMBRANE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "aurelian/capability/cap_chain.h"

namespace aurelian {

// Returns an empty string if the op is permitted, or a JSON broken reason
// (`{"error":"cap-..."}`) if denied. `has_anchor` false means the membrane has
// no provisioned anchor → nothing is trusted. `is_mutating` is the caller's
// (membrane-specific) classification of whether `verb` writes.
std::string EnforceCap(const std::vector<CapLink>& chain,
                       const PubKey& trusted_anchor,
                       bool has_anchor,
                       const std::string& verb,
                       bool is_mutating,
                       const std::string& target,
                       int64_t now_unix);

}  // namespace aurelian

#endif  // AURELIAN_CAPABILITY_CAP_MEMBRANE_H_
