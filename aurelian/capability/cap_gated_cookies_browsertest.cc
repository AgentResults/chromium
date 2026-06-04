// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C8.g — the browser membrane gates a REAL cookie write: a mode=read
// cap blocks the write (cookie stays absent); a write cap permits it.

#include "aurelian/capability/cap_gated_cookies.h"

#include "aurelian/capability/cap_chain.h"
#include "aurelian/handles/network/network_handle.h"
#include "base/time/time.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "content/public/test/browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace aurelian {

class AurelianCapGatedCookieBrowserTest : public InProcessBrowserTest {
 protected:
  content::BrowserContext* Ctx() { return browser()->profile(); }
  int64_t Now() { return base::Time::Now().ToTimeT(); }

  CapLink Mint(const std::string& predicate,
               const PrivKey& issuer,
               const PubKey& subject) {
    CapLink l;
    l.cap_id = "g";
    l.predicate = predicate;
    l.issuer_pub = PubFromPriv(issuer);
    l.subject_pub = subject;
    EXPECT_TRUE(SignLink(&l, issuer));
    return l;
  }
};

IN_PROC_BROWSER_TEST_F(AurelianCapGatedCookieBrowserTest, ReadCapBlocksWrite) {
  PrivKey anchor;
  anchor.fill(21);
  PrivKey chromium;
  chromium.fill(22);
  PubKey anchor_pub = PubFromPriv(anchor);

  const std::string url = "https://capgate.example.com/";

  // A read-only cap must NOT let the cookie write through.
  std::vector<CapLink> read_chain = {
      Mint("mode=read", anchor, PubFromPriv(chromium))};
  std::string denied = GatedSetCookie(Ctx(), anchor_pub, /*has_anchor=*/true,
                                      read_chain, url, "k", "v", Now());
  EXPECT_NE(denied.find("mode-read"), std::string::npos) << denied;
  EXPECT_FALSE(GetCookie(Ctx(), url, "k").ok)
      << "cookie was set despite a read-only cap";

  // A write cap permits the write; the cookie is now present.
  std::vector<CapLink> write_chain = {
      Mint("mode=write", anchor, PubFromPriv(chromium))};
  std::string ok = GatedSetCookie(Ctx(), anchor_pub, true, write_chain, url,
                                  "k", "v", Now());
  EXPECT_EQ(ok, "");
  auto got = GetCookie(Ctx(), url, "k");
  EXPECT_TRUE(got.ok);
  EXPECT_EQ(got.value, "v");
}

}  // namespace aurelian
