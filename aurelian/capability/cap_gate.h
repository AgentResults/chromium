// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian ACM-8: the federation-seam cap gate (design section 5
// prove-or-build). PROVEN before building: descendant-scoped enforcement for
// deep mirror URIs happened NOWHERE on the proven path — the browser side
// verified no cap, Agrippa's RegisteredEmbodimentHandle forwards every
// dispatchAt blindly, and Agrippa's mint gate is facet-granularity. This gate
// is the BUILD half: it runs at the AurelianBootstrap dispatchAt seam, BEFORE
// the dispatch, from the existing components — cap_wire chain decode +
// the one EnforceCap every membrane runs (C8.e: chain verify back to the
// operator anchor + effective-predicate enforcement).

#ifndef AURELIAN_CAPABILITY_CAP_GATE_H_
#define AURELIAN_CAPABILITY_CAP_GATE_H_

#include <cstdint>
#include <string>

#include "aurelian/capability/cap_chain.h"

namespace aurelian {

// Verifies + enforces a serialized delegation chain (cap_wire SerializeChain
// format) against a dispatch of `verb` at the full target `uri`. Returns the
// empty string if the dispatch is permitted, else the typed refusal reason
// "cap-refused:<inner>" — inner one of: cap-chain-malformed,
// cap-untrusted-anchor, cap-chain-invalid, cap-expired, cap-revoked,
// verb-not-permitted, mode-read, pattern-mismatch.
//
// Subtree convention: a cap "based at" B carries predicate pattern=B/*. The
// gate canonicalizes the target with a trailing slash on a second match
// attempt, so the base node ITSELF is admitted (B tested as "B/" matches
// "B/*") while a sibling id-prefix (BX) never is — no glob prefix-collision
// hole. Chain verify + predicate enforcement are the SAME EnforceCap every
// membrane runs; this seam adds only the wire decode, the canonicalization,
// and the federation verb classification (conservative: unknown verbs are
// mutating, so a mode=read cap denies them).
std::string GateFederationDispatch(const std::string& serialized_chain,
                                   const PubKey& trusted_anchor,
                                   bool has_anchor,
                                   const std::string& uri,
                                   const std::string& verb,
                                   int64_t now_unix);

}  // namespace aurelian

#endif  // AURELIAN_CAPABILITY_CAP_GATE_H_
