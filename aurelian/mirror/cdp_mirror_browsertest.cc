// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// ACM-1 browser tests (AURELIAN-GENERIC-CONTROL-TDD-PLAN Phase A): the
// CDP catalog mirror — read-only, data-driven, NO per-domain code —
// mounted behind the sealed legion://chrome/ root with its own
// EmbodimentPolicy row (design section 4: every projected node answers
// __getType / __getSchema / __getChildren behind the ONE sealed root).
// RED: nothing mounts `cdp` today — every dispatch below answers
// broken:out-of-scope. GREEN: ONE CdpMirror class projecting the
// embedded descriptor (ACM-S2's CdpCatalog).

#include "aurelian/catalog/cdp_catalog.h"
#include "aurelian/handles/root/root_handle.h"
#include "aurelian/membrane/embodiment_policy.h"
#include "base/json/json_reader.h"
#include "base/values.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "content/public/test/browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace aurelian {

namespace {

// Parses a serialized wire reply as a JSON list (the canonical-JSON form
// SerializeWireReply gives every non-scalar value).
std::optional<base::ListValue> ParseList(const std::string& reply) {
  return base::JSONReader::ReadList(reply, base::JSON_PARSE_RFC);
}

std::optional<base::DictValue> ParseDict(const std::string& reply) {
  return base::JSONReader::ReadDict(reply, base::JSON_PARSE_RFC);
}

bool ListContainsString(const base::ListValue& list, const std::string& s) {
  for (const base::Value& v : list) {
    if (v.is_string() && v.GetString() == s) {
      return true;
    }
  }
  return false;
}

}  // namespace

class AurelianCdpMirrorBrowserTest : public InProcessBrowserTest {};

// The mirror root enumerates EVERY descriptor domain as a child URI —
// the full catalog, not a hand-picked subset (>= 40 per the plan's RED;
// exactly the embedded descriptor's count by construction).
IN_PROC_BROWSER_TEST_F(AurelianCdpMirrorBrowserTest, EnumeratesAllDomains) {
  ChromeRoot* root = CreateChromeRoot();
  ASSERT_NE(root, nullptr);

  EXPECT_EQ(RootDispatch(root, "cdp/__getIdentity"), "legion://chrome/cdp");

  std::string reply = RootDispatch(root, "cdp/__getChildren");
  std::optional<base::ListValue> children = ParseList(reply);
  ASSERT_TRUE(children.has_value()) << "not a JSON list: " << reply;

  const CdpCatalog& cat = CdpCatalog::Get();
  EXPECT_EQ(children->size(), cat.domain_count()) << reply;
  EXPECT_GE(children->size(), 40u);
  for (const base::Value& c : *children) {
    ASSERT_TRUE(c.is_string()) << reply;
    EXPECT_EQ(c.GetString().rfind("legion://chrome/cdp/", 0), 0u)
        << c.GetString();
  }
  EXPECT_TRUE(ListContainsString(*children, "legion://chrome/cdp/Page"));
  EXPECT_TRUE(ListContainsString(*children, "legion://chrome/cdp/Browser"));
  // The fork-added domain is projected like every upstream one (data-
  // driven: it is in the descriptor, so it is in the mirror).
  EXPECT_TRUE(ListContainsString(*children, "legion://chrome/cdp/Asmodeus"));

  DestroyChromeRoot(root);
}

// A domain node enumerates its commands AND events as children, straight
// from the descriptor.
IN_PROC_BROWSER_TEST_F(AurelianCdpMirrorBrowserTest,
                       DomainEnumeratesCommandsAndEvents) {
  ChromeRoot* root = CreateChromeRoot();
  ASSERT_NE(root, nullptr);

  EXPECT_EQ(RootDispatch(root, "cdp/Page/__getIdentity"),
            "legion://chrome/cdp/Page");

  std::string reply = RootDispatch(root, "cdp/Page/__getChildren");
  std::optional<base::ListValue> children = ParseList(reply);
  ASSERT_TRUE(children.has_value()) << "not a JSON list: " << reply;

  // Page carries dozens of commands + events; the exact count is the
  // descriptor's business (roll-stable assert).
  EXPECT_GT(children->size(), 60u) << reply;
  EXPECT_TRUE(ListContainsString(*children,
                                 "legion://chrome/cdp/Page/navigate"))
      << reply;  // a command
  EXPECT_TRUE(ListContainsString(*children,
                                 "legion://chrome/cdp/Page/frameNavigated"))
      << reply;  // an event
  for (const base::Value& c : *children) {
    ASSERT_TRUE(c.is_string()) << reply;
    EXPECT_EQ(c.GetString().rfind("legion://chrome/cdp/Page/", 0), 0u)
        << c.GetString();
  }

  DestroyChromeRoot(root);
}

// A command node's __getSchema IS the descriptor's typed entry —
// Page.navigate carries parameters with url:string (the same schema
// ACM-S2 pinned readable from the catalog).
IN_PROC_BROWSER_TEST_F(AurelianCdpMirrorBrowserTest,
                       CommandSchemaFromDescriptor) {
  ChromeRoot* root = CreateChromeRoot();
  ASSERT_NE(root, nullptr);

  std::string reply = RootDispatch(root, "cdp/Page/navigate/__getSchema");
  std::optional<base::DictValue> schema = ParseDict(reply);
  ASSERT_TRUE(schema.has_value()) << "not a JSON dict: " << reply;

  const base::ListValue* params = schema->FindList("parameters");
  ASSERT_NE(params, nullptr) << reply;
  bool saw_url_string = false;
  for (const base::Value& p : *params) {
    const base::DictValue* pd = p.GetIfDict();
    ASSERT_NE(pd, nullptr);
    const std::string* name = pd->FindString("name");
    const std::string* type = pd->FindString("type");
    if (name && *name == "url" && type && *type == "string") {
      saw_url_string = true;
    }
  }
  EXPECT_TRUE(saw_url_string) << "url:string not in mirror schema: " << reply;

  DestroyChromeRoot(root);
}

// Every node stamps its declared Layer-1 type (design section 6) — and
// the domain stamp is asserted for ALL descriptor domains, which is what
// "data-driven, no per-domain code" means observably: no domain can take
// a divergent code path.
IN_PROC_BROWSER_TEST_F(AurelianCdpMirrorBrowserTest, NodesCarryDeclaredTypes) {
  ChromeRoot* root = CreateChromeRoot();
  ASSERT_NE(root, nullptr);

  EXPECT_EQ(RootDispatch(root, "cdp/__getType"), "legion://types/CdpCatalog");
  const CdpCatalog& cat = CdpCatalog::Get();
  for (const std::string& d : cat.domains()) {
    EXPECT_EQ(RootDispatch(root, "cdp/" + d + "/__getType"),
              "legion://types/CdpDomain")
        << d;
  }
  EXPECT_EQ(RootDispatch(root, "cdp/Page/navigate/__getType"),
            "legion://types/CdpCommand");
  EXPECT_EQ(RootDispatch(root, "cdp/Browser/getVersion/__getType"),
            "legion://types/CdpCommand");
  EXPECT_EQ(RootDispatch(root, "cdp/Page/frameNavigated/__getType"),
            "legion://types/CdpEvent");

  DestroyChromeRoot(root);
}

// The domain node's __getSchema carries the DERIVED session-context
// annotation (design section 2: the catalog annotates each domain; the
// rows come from ACM-S2's derivation, never hand-named here).
IN_PROC_BROWSER_TEST_F(AurelianCdpMirrorBrowserTest,
                       DomainSchemaCarriesSessionContext) {
  ChromeRoot* root = CreateChromeRoot();
  ASSERT_NE(root, nullptr);

  std::optional<base::DictValue> page =
      ParseDict(RootDispatch(root, "cdp/Page/__getSchema"));
  ASSERT_TRUE(page.has_value());
  const base::DictValue* page_ctx = page->FindDict("sessionContext");
  ASSERT_NE(page_ctx, nullptr);
  EXPECT_EQ(page_ctx->FindBool("inBrowserUnion"), false);

  std::optional<base::DictValue> browser =
      ParseDict(RootDispatch(root, "cdp/Browser/__getSchema"));
  ASSERT_TRUE(browser.has_value());
  const base::DictValue* browser_ctx = browser->FindDict("sessionContext");
  ASSERT_NE(browser_ctx, nullptr);
  EXPECT_EQ(browser_ctx->FindBool("inBrowserUnion"), true);

  // The derived browser-only annotation rides through too (membership is
  // the derivation's, asserted via the catalog, not a hand-named list).
  for (const std::string& d : CdpCatalog::Get().BrowserOnlyDomains()) {
    std::optional<base::DictValue> dom =
        ParseDict(RootDispatch(root, "cdp/" + d + "/__getSchema"));
    ASSERT_TRUE(dom.has_value()) << d;
    const base::DictValue* ctx = dom->FindDict("sessionContext");
    ASSERT_NE(ctx, nullptr) << d;
    EXPECT_EQ(ctx->FindBool("browserOnly"), true) << d;
  }

  DestroyChromeRoot(root);
}

// Unknown names under the mirror answer TYPED reasons, not silence and
// not a generic out-of-scope (catalog-lookup honesty).
IN_PROC_BROWSER_TEST_F(AurelianCdpMirrorBrowserTest, UnknownNamesTyped) {
  ChromeRoot* root = CreateChromeRoot();
  ASSERT_NE(root, nullptr);

  EXPECT_EQ(RootDispatch(root, "cdp/NoSuchDomain/__getType"),
            "broken:unknown-domain");
  EXPECT_EQ(RootDispatch(root, "cdp/Page/noSuchMember/__getType"),
            "broken:unknown-command-or-event");

  DestroyChromeRoot(root);
}

// The seal holds: a policy that does not grant `cdp` refuses the whole
// mirror subtree (the per-slice EmbodimentPolicy row, design section 7
// review N4 — ACM-1 adds `cdp` to FullStandalone in its own GREEN).
IN_PROC_BROWSER_TEST_F(AurelianCdpMirrorBrowserTest, MirrorSealedBehindPolicy) {
  ChromeRoot* sealed_off = CreateChromeRootWithPolicy(
      EmbodimentPolicy::WithCapabilities({"system"}));
  ASSERT_NE(sealed_off, nullptr);
  EXPECT_NE(RootDispatch(sealed_off, "cdp").find("broken"), std::string::npos);
  EXPECT_NE(RootDispatch(sealed_off, "cdp/__getChildren").find("broken"),
            std::string::npos);
  EXPECT_NE(
      RootDispatch(sealed_off, "cdp/Page/navigate/__getSchema").find("broken"),
      std::string::npos);
  DestroyChromeRoot(sealed_off);

  ChromeRoot* sealed_in = CreateChromeRoot();  // FullStandalone grants cdp.
  ASSERT_NE(sealed_in, nullptr);
  EXPECT_EQ(RootDispatch(sealed_in, "cdp/__getIdentity"),
            "legion://chrome/cdp");
  DestroyChromeRoot(sealed_in);
}

}  // namespace aurelian
