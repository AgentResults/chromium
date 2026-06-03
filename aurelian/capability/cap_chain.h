// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C8.b: cap-token signed chain-of-trust (the TRUST half). Each link is
// an attenuated delegation signed (Ed25519) by the delegating membrane's key.
// A membrane receiving a cap verifies the WHOLE chain back to a trusted anchor
// before any dispatch — no sender vouching. Per AURELIAN-DESIGN.md §11.5.

#ifndef AURELIAN_CAPABILITY_CAP_CHAIN_H_
#define AURELIAN_CAPABILITY_CAP_CHAIN_H_

#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "aurelian/capability/cap_predicate.h"

namespace aurelian {

using PubKey = std::array<uint8_t, 32>;
using PrivKey = std::array<uint8_t, 32>;
using Signature = std::array<uint8_t, 64>;

// One delegation link. `issuer_pub` is the delegating membrane's key (it signs
// the link); `subject_pub` is the membrane the cap is granted to. The root
// link's issuer is the operator trust anchor.
struct CapLink {
  std::string cap_id;
  std::string parent_cap_id;  // empty for the root link
  std::string predicate;      // bracketed predicate body (see CapPredicate)
  int64_t expires = 0;        // unix seconds; 0 = never
  PubKey issuer_pub{};
  PubKey subject_pub{};
  Signature signature{};
};

// Derives a public key from a private seed.
PubKey PubFromPriv(const PrivKey& priv);

// The canonical byte string a link's signature covers.
std::vector<uint8_t> CanonicalLinkBytes(const CapLink& link);

// Signs `link` with `issuer_priv` (must match link.issuer_pub), writing the
// signature. Returns false on key mismatch.
bool SignLink(CapLink* link, const PrivKey& issuer_priv);

struct ChainVerifyResult {
  bool ok = false;
  // "ok" | "cap-chain-invalid" | "cap-untrusted-anchor" | "cap-revoked" |
  // "cap-expired"
  std::string reason;
  // The most-attenuated (effective) predicate, valid only when ok.
  CapPredicate effective;
};

// Verifies `chain` (root first) back to `trusted_anchor_pub`:
//  - the root link's issuer must be the trusted anchor (else untrusted-anchor),
//  - every link's signature must verify under its issuer key,
//  - every non-root link's issuer must be the previous link's subject and its
//    parent_cap_id must name the previous link (else chain-invalid),
//  - every link's predicate must be a strict reduction of its parent's,
//  - no link may be expired at `now_unix` (else cap-expired),
//  - no link's cap_id may be in `revoked` (else cap-revoked).
ChainVerifyResult VerifyChain(const std::vector<CapLink>& chain,
                              const PubKey& trusted_anchor_pub,
                              int64_t now_unix,
                              const std::set<std::string>& revoked);

}  // namespace aurelian

#endif  // AURELIAN_CAPABILITY_CAP_CHAIN_H_
