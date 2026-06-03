// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/capability/cap_chain.h"

#include "testing/gtest/include/gtest/gtest.h"

namespace aurelian {
namespace {

// Deterministic test seed (no secure randomness needed in a unit test).
PrivKey Seed(uint8_t b) {
  PrivKey k{};
  k.fill(b);
  return k;
}

CapLink MakeLink(const std::string& cap_id,
                 const std::string& parent,
                 const std::string& predicate,
                 int64_t expires,
                 const PrivKey& issuer_priv,
                 const PubKey& subject_pub) {
  CapLink link;
  link.cap_id = cap_id;
  link.parent_cap_id = parent;
  link.predicate = predicate;
  link.expires = expires;
  link.issuer_pub = PubFromPriv(issuer_priv);
  link.subject_pub = subject_pub;
  EXPECT_TRUE(SignLink(&link, issuer_priv));
  return link;
}

// operator anchor -> machine(k1) -> renderer(k2)
struct Fixture {
  PrivKey anchor = Seed(1);
  PrivKey k1 = Seed(2);
  PrivKey k2 = Seed(3);
  PubKey anchor_pub = PubFromPriv(anchor);
  PubKey k1_pub = PubFromPriv(k1);
  PubKey k2_pub = PubFromPriv(k2);

  // L0: anchor grants machine a write scope. L1: machine delegates a reduced
  // read scope to the renderer.
  std::vector<CapLink> ValidChain() {
    return {
        MakeLink("root", "", "mode=write,verbs=text|html|setText", 0, anchor,
                 k1_pub),
        MakeLink("r1", "root", "mode=read,verbs=text", 0, k1, k2_pub),
    };
  }
};

TEST(AurelianCapChainTest, ValidChainVerifies) {
  Fixture f;
  ChainVerifyResult r =
      VerifyChain(f.ValidChain(), f.anchor_pub, /*now=*/1000, {});
  EXPECT_TRUE(r.ok) << r.reason;
  EXPECT_EQ(r.reason, "ok");
  EXPECT_EQ(r.effective.mode, CapPredicate::Mode::kRead);
  ASSERT_EQ(r.effective.verbs.size(), 1u);
  EXPECT_EQ(r.effective.verbs[0], "text");
}

TEST(AurelianCapChainTest, UnanchoredChainRejected) {
  Fixture f;
  PubKey untrusted = PubFromPriv(Seed(9));
  ChainVerifyResult r = VerifyChain(f.ValidChain(), untrusted, 1000, {});
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(r.reason, "cap-untrusted-anchor");
}

TEST(AurelianCapChainTest, ForgedSignatureRejected) {
  Fixture f;
  std::vector<CapLink> chain = f.ValidChain();
  chain[1].signature[0] ^= 0xFF;  // tamper the renderer link's signature
  ChainVerifyResult r = VerifyChain(chain, f.anchor_pub, 1000, {});
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(r.reason, "cap-chain-invalid");
}

TEST(AurelianCapChainTest, BroadeningLinkRejected) {
  Fixture f;
  std::vector<CapLink> chain = {
      MakeLink("root", "", "mode=read,verbs=text", 0, f.anchor, f.k1_pub),
      // Child tries to broaden read -> write: not a strict reduction.
      MakeLink("r1", "root", "mode=write", 0, f.k1, f.k2_pub),
  };
  ChainVerifyResult r = VerifyChain(chain, f.anchor_pub, 1000, {});
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(r.reason, "cap-chain-invalid");
}

TEST(AurelianCapChainTest, WrongDelegatingKeyRejected) {
  Fixture f;
  // L1 is correctly signed, but by the anchor instead of the machine key, so
  // its issuer != the previous link's subject.
  std::vector<CapLink> chain = {
      MakeLink("root", "", "mode=write,verbs=text", 0, f.anchor, f.k1_pub),
      MakeLink("r1", "root", "mode=read,verbs=text", 0, f.anchor, f.k2_pub),
  };
  ChainVerifyResult r = VerifyChain(chain, f.anchor_pub, 1000, {});
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(r.reason, "cap-chain-invalid");
}

TEST(AurelianCapChainTest, RevokedCapRejected) {
  Fixture f;
  ChainVerifyResult r =
      VerifyChain(f.ValidChain(), f.anchor_pub, 1000, {"r1"});
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(r.reason, "cap-revoked");
}

TEST(AurelianCapChainTest, ExpiredLinkRejected) {
  Fixture f;
  std::vector<CapLink> chain = {
      MakeLink("root", "", "mode=write,verbs=text", 0, f.anchor, f.k1_pub),
      MakeLink("r1", "root", "mode=read,verbs=text", /*expires=*/500, f.k1,
               f.k2_pub),
  };
  ChainVerifyResult r = VerifyChain(chain, f.anchor_pub, /*now=*/1000, {});
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(r.reason, "cap-expired");
}

}  // namespace
}  // namespace aurelian
