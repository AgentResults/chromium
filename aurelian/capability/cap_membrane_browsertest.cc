// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C8.c browser tests — the renderer membrane verifies a cap chain
// independently (against the anchor IT was provisioned) and enforces the
// effective predicate, BEFORE any DOM op. Real multi-process renderer.

#include "aurelian/capability/cap_chain.h"
#include "aurelian/capability/cap_wire.h"
#include "aurelian/public/mojom/aurelian_wire.mojom.h"

#include <cstdlib>

#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/time/time.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"
#include "url/gurl.h"

namespace aurelian {

class AurelianCapBrowserTest : public InProcessBrowserTest {
 protected:
  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    anchor_.fill(7);
    machine_.fill(8);
    anchor_pub_ = PubFromPriv(anchor_);
    machine_pub_ = PubFromPriv(machine_);
  }

  void Navigate() {
    ASSERT_TRUE(ui_test_utils::NavigateToURL(
        browser(),
        GURL("data:text/html,<div id='t'>cap target</div>")));
  }

  content::RenderFrameHost* MainFrame() {
    return browser()
        ->tab_strip_model()
        ->GetActiveWebContents()
        ->GetPrimaryMainFrame();
  }

  void GetWire(mojo::AssociatedRemote<mojom::AurelianWire>* wire) {
    MainFrame()->GetRemoteAssociatedInterfaces()->GetInterface(wire);
  }

  // Provisions the renderer membrane's trust anchor (what the browser would do
  // once at boot from the Agrippa-published key).
  void ProvisionAnchor(mojo::AssociatedRemote<mojom::AurelianWire>* wire,
                       const PubKey& anchor_pub) {
    (*wire)->SetTrustAnchor(
        std::vector<uint8_t>(anchor_pub.begin(), anchor_pub.end()));
    (*wire).FlushForTesting();  // ensure the renderer applied it
  }

  // A single-link chain: the anchor grants the frame a cap with `predicate`.
  std::vector<CapLink> Grant(const std::string& predicate, int64_t expires) {
    CapLink l;
    l.cap_id = "root";
    l.predicate = predicate;
    l.expires = expires;
    l.issuer_pub = anchor_pub_;
    l.subject_pub = machine_pub_;
    EXPECT_TRUE(SignLink(&l, anchor_));
    return {l};
  }

  std::string DispatchWithCap(mojo::AssociatedRemote<mojom::AurelianWire>* wire,
                              const std::vector<CapLink>& chain,
                              const std::string& verb,
                              const std::string& param) {
    std::vector<uint8_t> envelope = EncodeCapEnvelope(chain, verb, param);
    std::string out;
    base::RunLoop loop;
    (*wire)->Dispatch(
        envelope, base::BindOnce(
                      [](base::RunLoop* l, std::string* o,
                         const std::vector<uint8_t>& reply) {
                        *o = std::string(reply.begin(), reply.end());
                        l->Quit();
                      },
                      &loop, &out));
    loop.Run();
    return out;
  }

  std::string QueryFirstNodeId(
      mojo::AssociatedRemote<mojom::AurelianWire>* wire) {
    // Plain (no-cap) dispatch to resolve a node id.
    std::string verb = "dom.query\tdiv";
    std::vector<uint8_t> env(verb.begin(), verb.end());
    std::string out;
    base::RunLoop loop;
    (*wire)->Dispatch(env, base::BindOnce(
                               [](base::RunLoop* l, std::string* o,
                                  const std::vector<uint8_t>& reply) {
                                 *o = std::string(reply.begin(), reply.end());
                                 l->Quit();
                               },
                               &loop, &out));
    loop.Run();
    // out is like "[3]"; strip the brackets.
    if (out.size() >= 3 && out.front() == '[') {
      return out.substr(1, out.size() - 2);
    }
    return std::string();
  }

  int64_t Now() { return base::Time::Now().ToTimeT(); }

  PrivKey anchor_{};
  PrivKey machine_{};
  PubKey anchor_pub_{};
  PubKey machine_pub_{};
};

// A [mode=read] cap permits a read verb but rejects a DOM mutation, in the
// renderer membrane.
IN_PROC_BROWSER_TEST_F(AurelianCapBrowserTest, ModeReadBlocksDomMutation) {
  Navigate();
  mojo::AssociatedRemote<mojom::AurelianWire> wire;
  GetWire(&wire);
  ProvisionAnchor(&wire, anchor_pub_);

  std::string node = QueryFirstNodeId(&wire);
  ASSERT_FALSE(node.empty());

  std::vector<CapLink> chain = Grant("mode=read", /*expires=*/0);

  // Read is allowed.
  std::string read = DispatchWithCap(&wire, chain, "dom.node.text", node);
  EXPECT_EQ(read.find("error"), std::string::npos) << read;
  EXPECT_NE(read.find("cap target"), std::string::npos) << read;

  // Mutation is denied with mode-read.
  std::string mutate = DispatchWithCap(
      &wire, chain, "dom.node.setAttribute", node + "\tdata-x\t1");
  EXPECT_NE(mutate.find("mode-read"), std::string::npos) << mutate;
}

// A forged signature is rejected by the renderer even though the hub forwarded
// the cap.
IN_PROC_BROWSER_TEST_F(AurelianCapBrowserTest,
                       RendererVerifiesChainIndependently) {
  Navigate();
  mojo::AssociatedRemote<mojom::AurelianWire> wire;
  GetWire(&wire);
  ProvisionAnchor(&wire, anchor_pub_);

  std::string node = QueryFirstNodeId(&wire);
  ASSERT_FALSE(node.empty());

  std::vector<CapLink> chain = Grant("mode=write", 0);
  chain[0].signature[0] ^= 0xFF;  // forge

  std::string reply =
      DispatchWithCap(&wire, chain, "dom.node.text", node);
  EXPECT_NE(reply.find("cap-chain-invalid"), std::string::npos) << reply;
}

// An expired cap is rejected over Mojo.
IN_PROC_BROWSER_TEST_F(AurelianCapBrowserTest, ExpiredCapRejectedOverMojo) {
  Navigate();
  mojo::AssociatedRemote<mojom::AurelianWire> wire;
  GetWire(&wire);
  ProvisionAnchor(&wire, anchor_pub_);

  std::string node = QueryFirstNodeId(&wire);
  ASSERT_FALSE(node.empty());

  // Expires well in the past.
  std::vector<CapLink> chain = Grant("mode=write", /*expires=*/1000);
  std::string reply = DispatchWithCap(&wire, chain, "dom.node.text", node);
  EXPECT_NE(reply.find("cap-expired"), std::string::npos) << reply;
}

// A chain anchored to a DIFFERENT key than the membrane was provisioned with is
// rejected as untrusted.
IN_PROC_BROWSER_TEST_F(AurelianCapBrowserTest, WrongAnchorRejected) {
  Navigate();
  mojo::AssociatedRemote<mojom::AurelianWire> wire;
  GetWire(&wire);

  // Provision a DIFFERENT anchor than the chain is signed under.
  PrivKey other;
  other.fill(42);
  ProvisionAnchor(&wire, PubFromPriv(other));

  std::string node = QueryFirstNodeId(&wire);
  ASSERT_FALSE(node.empty());

  std::vector<CapLink> chain = Grant("mode=write", 0);
  std::string reply = DispatchWithCap(&wire, chain, "dom.node.text", node);
  EXPECT_NE(reply.find("cap-untrusted-anchor"), std::string::npos) << reply;
}

// AU-CAP-LIVE — the operator anchor is provisioned at BOOT (env
// AURELIAN_CAP_ANCHOR, what Agrippa publishes), NOT by a manual ProvisionAnchor,
// so the already-real cap crypto actually runs in production. A cap signed by the
// matching operator key is ENFORCED; a forged chain rooted at a different key is
// REFUSED. RED before CapAnchorProvisioner is wired into boot (no anchor ->
// every op is cap-untrusted-anchor).
class AurelianCapBootAnchorBrowserTest : public AurelianCapBrowserTest {
 protected:
  void SetUpInProcessBrowserTestFixture() override {
    AurelianCapBrowserTest::SetUpInProcessBrowserTestFixture();
    // Publish the operator anchor BEFORE the browser process boots.
    PrivKey anchor;
    anchor.fill(7);  // matches AurelianCapBrowserTest::anchor_
    PubKey pub = PubFromPriv(anchor);
    ::setenv("AURELIAN_CAP_ANCHOR", base::HexEncode(pub).c_str(),
             /*overwrite=*/1);
  }
  void TearDownInProcessBrowserTestFixture() override {
    ::unsetenv("AURELIAN_CAP_ANCHOR");
    AurelianCapBrowserTest::TearDownInProcessBrowserTestFixture();
  }
};

IN_PROC_BROWSER_TEST_F(AurelianCapBootAnchorBrowserTest,
                       BootAnchorEnforcesSignedCapAndRefusesForgery) {
  Navigate();
  mojo::AssociatedRemote<mojom::AurelianWire> wire;
  GetWire(&wire);  // NO ProvisionAnchor — the boot provisioner did it.

  std::string node = QueryFirstNodeId(&wire);
  ASSERT_FALSE(node.empty());

  // A cap signed by the boot-provisioned operator anchor verifies, so the read
  // is permitted (NOT refused as cap-untrusted-anchor).
  std::vector<CapLink> good = Grant("mode=read", /*expires=*/0);
  std::string ok = DispatchWithCap(&wire, good, "dom.node.text", node);
  EXPECT_EQ(ok.find("cap-untrusted-anchor"), std::string::npos)
      << "boot anchor must make the operator-signed cap verify live: " << ok;
  EXPECT_NE(ok.find("cap target"), std::string::npos) << ok;

  // A chain rooted at a DIFFERENT key is refused by the live boot anchor.
  PrivKey forger;
  forger.fill(9);
  CapLink forged;
  forged.cap_id = "root";
  forged.predicate = "mode=read";
  forged.expires = 0;
  forged.issuer_pub = PubFromPriv(forger);
  forged.subject_pub = machine_pub_;
  ASSERT_TRUE(SignLink(&forged, forger));
  std::string bad = DispatchWithCap(&wire, {forged}, "dom.node.text", node);
  EXPECT_NE(bad.find("cap-untrusted-anchor"), std::string::npos)
      << "a chain not rooted at the boot anchor must be refused: " << bad;
}

// AU-CAP-PROV-TEST — a DEDICATED test of CapAnchorProvisioner's documented
// wrong-sized-anchor no-op (header: "A wrong-sized anchor makes this a no-op (the
// membrane simply never gains an anchor)"). A MALFORMED boot anchor must NOT be
// provisioned onto the membrane, so EVERY cap — even one correctly signed by the
// real operator key — fails closed. This proves the provisioner validates the
// anchor before SetTrustAnchor (a malformed boot key cannot silently grant). The
// positive path is the sibling BootAnchorEnforcesSignedCapAndRefusesForgery.
class AurelianCapBootAnchorWrongSizeBrowserTest : public AurelianCapBrowserTest {
 protected:
  void SetUpInProcessBrowserTestFixture() override {
    AurelianCapBrowserTest::SetUpInProcessBrowserTestFixture();
    // 16 hex bytes = wrong size (the anchor must be a 32-byte Ed25519 pubkey).
    ::setenv("AURELIAN_CAP_ANCHOR", "00112233445566778899aabbccddeeff",
             /*overwrite=*/1);
  }
  void TearDownInProcessBrowserTestFixture() override {
    ::unsetenv("AURELIAN_CAP_ANCHOR");
    AurelianCapBrowserTest::TearDownInProcessBrowserTestFixture();
  }
};

IN_PROC_BROWSER_TEST_F(AurelianCapBootAnchorWrongSizeBrowserTest,
                       WrongSizedBootAnchorNoOpsSoCapsFailClosed) {
  Navigate();
  mojo::AssociatedRemote<mojom::AurelianWire> wire;
  GetWire(&wire);  // NO ProvisionAnchor; the boot provisioner saw a malformed anchor.

  std::string node = QueryFirstNodeId(&wire);
  ASSERT_FALSE(node.empty());

  // A cap correctly signed by the REAL operator anchor is still refused: the
  // provisioner no-op'd on the malformed boot key, so the membrane never gained
  // an anchor and fails closed (a bad boot key cannot accidentally grant access).
  std::vector<CapLink> good = Grant("mode=read", /*expires=*/0);
  std::string out = DispatchWithCap(&wire, good, "dom.node.text", node);
  EXPECT_NE(out.find("cap-untrusted-anchor"), std::string::npos)
      << "a wrong-sized boot anchor must no-op -> membrane anchorless -> fail closed: "
      << out;
}

}  // namespace aurelian
