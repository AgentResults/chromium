// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/capability/cap_chain.h"

#include <cstring>

#include "velite/crypto.hpp"

namespace aurelian {

CapLink::CapLink() = default;
CapLink::~CapLink() = default;
CapLink::CapLink(const CapLink&) = default;
CapLink& CapLink::operator=(const CapLink&) = default;
CapLink::CapLink(CapLink&&) = default;
CapLink& CapLink::operator=(CapLink&&) = default;

ChainVerifyResult::ChainVerifyResult() = default;
ChainVerifyResult::~ChainVerifyResult() = default;
ChainVerifyResult::ChainVerifyResult(const ChainVerifyResult&) = default;
ChainVerifyResult& ChainVerifyResult::operator=(const ChainVerifyResult&) =
    default;

namespace {

void AppendField(std::vector<uint8_t>* out, const std::string& s) {
  out->insert(out->end(), s.begin(), s.end());
  out->push_back('\0');
}

void AppendKey(std::vector<uint8_t>* out, const PubKey& k) {
  out->insert(out->end(), k.begin(), k.end());
}

}  // namespace

PubKey PubFromPriv(const PrivKey& priv) {
  PubKey pub{};
  velite::crypto::ed25519_pub_from_priv(priv.data(), pub.data());
  return pub;
}

std::vector<uint8_t> CanonicalLinkBytes(const CapLink& link) {
  // Deterministic serialization of every field EXCEPT the signature. Both the
  // issuer and subject keys are covered so neither can be swapped.
  std::vector<uint8_t> out;
  AppendField(&out, link.cap_id);
  AppendField(&out, link.parent_cap_id);
  AppendField(&out, link.predicate);
  AppendField(&out, std::to_string(link.expires));
  AppendKey(&out, link.issuer_pub);
  AppendKey(&out, link.subject_pub);
  return out;
}

bool SignLink(CapLink* link, const PrivKey& issuer_priv) {
  // issuer_pub must match the signing key, else the membrane that later
  // verifies against issuer_pub would reject our own signature.
  if (PubFromPriv(issuer_priv) != link->issuer_pub) {
    return false;
  }
  std::vector<uint8_t> msg = CanonicalLinkBytes(*link);
  return velite::crypto::ed25519_sign(issuer_priv.data(), link->issuer_pub.data(),
                                      msg.data(), msg.size(),
                                      link->signature.data());
}

ChainVerifyResult VerifyChain(const std::vector<CapLink>& chain,
                              const PubKey& trusted_anchor_pub,
                              int64_t now_unix,
                              const std::set<std::string>& revoked) {
  ChainVerifyResult result;
  auto fail = [&](const char* reason) {
    result.ok = false;
    result.reason = reason;
    result.effective = CapPredicate();
    return result;
  };

  if (chain.empty()) {
    return fail("cap-chain-invalid");
  }

  CapPredicate effective;
  for (size_t i = 0; i < chain.size(); ++i) {
    const CapLink& link = chain[i];

    // 1. Signature verifies under the link's own issuer key.
    std::vector<uint8_t> msg = CanonicalLinkBytes(link);
    if (!velite::crypto::ed25519_verify(link.issuer_pub.data(), msg.data(),
                                        msg.size(), link.signature.data())) {
      return fail("cap-chain-invalid");
    }

    // 2. Anchoring / parent binding.
    if (i == 0) {
      if (link.issuer_pub != trusted_anchor_pub) {
        return fail("cap-untrusted-anchor");
      }
    } else {
      const CapLink& parent = chain[i - 1];
      if (link.issuer_pub != parent.subject_pub ||
          link.parent_cap_id != parent.cap_id) {
        return fail("cap-chain-invalid");
      }
    }

    // 3. Revocation + expiry.
    if (revoked.count(link.cap_id)) {
      return fail("cap-revoked");
    }
    if (link.expires != 0 && now_unix > link.expires) {
      return fail("cap-expired");
    }

    // 4. Strict reduction of the parent predicate.
    CapPredicate pred = CapPredicate::Parse(link.predicate);
    if (i > 0 && !pred.IsReductionOf(effective)) {
      return fail("cap-chain-invalid");
    }
    effective = pred;
  }

  result.ok = true;
  result.reason = "ok";
  result.effective = effective;
  return result;
}

}  // namespace aurelian
