// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C8.e — the shared membrane check, exercised as the Chromium
// (browser-process) membrane honouring an Agrippa-signed machine grant that is
// attenuated to one subtree.

#include "aurelian/capability/cap_membrane.h"

#include "aurelian/capability/cap_chain.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace aurelian {
namespace {

PrivKey Seed(uint8_t b) {
  PrivKey k{};
  k.fill(b);
  return k;
}

CapLink Mint(const std::string& cap_id,
             const std::string& parent,
             const std::string& predicate,
             const PrivKey& issuer_priv,
             const PubKey& subject_pub) {
  CapLink l;
  l.cap_id = cap_id;
  l.parent_cap_id = parent;
  l.predicate = predicate;
  l.issuer_pub = PubFromPriv(issuer_priv);
  l.subject_pub = subject_pub;
  EXPECT_TRUE(SignLink(&l, issuer_priv));
  return l;
}

// operator/Agrippa anchor -> Chromium process domain.
TEST(AurelianCapMembraneTest, MachineGrantAttenuatesChromium) {
  PrivKey agrippa = Seed(11);  // operator trust anchor (machine key)
  PrivKey chromium = Seed(12);
  PubKey agrippa_pub = PubFromPriv(agrippa);
  PubKey chromium_pub = PubFromPriv(chromium);

  // Agrippa grants the Chromium domain a machine cap scoped to one subtree.
  std::vector<CapLink> chain = {
      Mint("machine-grant", "",
           "pattern=legion://chrome/profile/*", agrippa, chromium_pub)};

  // An in-scope op is permitted.
  EXPECT_EQ(EnforceCap(chain, agrippa_pub, /*has_anchor=*/true, "cookies.get",
                       /*is_mutating=*/false,
                       "legion://chrome/profile/cookies", /*now=*/1000),
            "");

  // An out-of-scope op (a file op outside the granted subtree) is blocked.
  std::string denied =
      EnforceCap(chain, agrippa_pub, true, "fs.read", false,
                 "legion://machine/fs/etc/passwd", 1000);
  EXPECT_NE(denied.find("pattern-mismatch"), std::string::npos) << denied;
}

// A full machine -> process -> renderer chain still enforces, and a renderer
// link that broadens the machine grant is rejected.
TEST(AurelianCapMembraneTest, NestedDomainChainEnforced) {
  PrivKey agrippa = Seed(11);
  PrivKey chromium = Seed(12);
  PrivKey renderer = Seed(13);
  PubKey agrippa_pub = PubFromPriv(agrippa);

  std::vector<CapLink> chain = {
      Mint("machine", "", "mode=write,pattern=legion://chrome/*", agrippa,
           PubFromPriv(chromium)),
      Mint("process", "machine",
           "mode=read,pattern=legion://chrome/browser/tabs/*", chromium,
           PubFromPriv(renderer)),
  };

  // In-scope read under the attenuated process cap.
  EXPECT_EQ(EnforceCap(chain, agrippa_pub, true, "tab.url", false,
                       "legion://chrome/browser/tabs/7", 1000),
            "");
  // A write is denied (process cap reduced to read).
  EXPECT_NE(EnforceCap(chain, agrippa_pub, true, "tab.close", true,
                       "legion://chrome/browser/tabs/7", 1000)
                .find("mode-read"),
            std::string::npos);

  // If the membrane has no anchor, nothing is trusted.
  PubKey none{};
  EXPECT_NE(EnforceCap(chain, none, /*has_anchor=*/false, "tab.url", false,
                       "legion://chrome/browser/tabs/7", 1000)
                .find("cap-untrusted-anchor"),
            std::string::npos);
}

}  // namespace
}  // namespace aurelian
