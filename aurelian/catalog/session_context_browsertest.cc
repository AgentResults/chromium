// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// ACM-S2 browser tests (AURELIAN-GENERIC-CONTROL-TDD-PLAN Phase 0):
// the embedded descriptor + the DERIVED session-context table + the
// AUTHORED per-command override list, and — load-bearing — the LIVE
// PINS that arbitrate browser-only membership against the real host
// (design section 2 round-8: the pin, not the prose, is the arbiter).

#include "aurelian/catalog/cdp_catalog.h"

#include "aurelian/handles/browser/devtools_handle.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace aurelian {

class AurelianSessionContextBrowserTest : public InProcessBrowserTest {
 protected:
  content::WebContents* GetWC() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }

  void NavigateToFixture() {
    ASSERT_TRUE(ui_test_utils::NavigateToURL(
        browser(), GURL("data:text/html,<title>sct</title>fixture")));
  }
};

// The embedded descriptor enumerates the full merged protocol —
// version-locked to THIS binary (design section 2).
IN_PROC_BROWSER_TEST_F(AurelianSessionContextBrowserTest, DescriptorCounts) {
  const CdpCatalog& cat = CdpCatalog::Get();
  EXPECT_EQ(cat.domain_count(), 57u);
  EXPECT_EQ(cat.command_count(), 718u);
  EXPECT_EQ(cat.event_count(), 250u);
}

// Page.navigate's parameter schema is readable from the descriptor.
IN_PROC_BROWSER_TEST_F(AurelianSessionContextBrowserTest,
                       CommandSchemaReadable) {
  const CdpCatalog& cat = CdpCatalog::Get();
  const base::DictValue* cmd = cat.FindCommand("Page", "navigate");
  ASSERT_NE(cmd, nullptr);
  const base::ListValue* params = cmd->FindList("parameters");
  ASSERT_NE(params, nullptr);
  bool saw_url_string = false;
  for (const base::Value& p : *params) {
    const base::DictValue& pd = p.GetDict();
    const std::string* name = pd.FindString("name");
    const std::string* type = pd.FindString("type");
    if (name && *name == "url" && type && *type == "string") {
      saw_url_string = true;
    }
  }
  EXPECT_TRUE(saw_url_string) << "Page.navigate url:string not readable";
}

// Every descriptor domain — all 57, including the fork-added Asmodeus,
// which registers ONLY at the chrome layer — has exactly one derived
// session-context row.
IN_PROC_BROWSER_TEST_F(AurelianSessionContextBrowserTest,
                       EveryDomainHasDerivedRow) {
  const CdpCatalog& cat = CdpCatalog::Get();
  for (const std::string& d : cat.domains()) {
    EXPECT_TRUE(cat.ContextFor(d).has_value())
        << "descriptor domain with no derived row: " << d;
  }
  // The fork-added domain is present AND classified (derivable only
  // because site (3) — chrome_devtools_session.cc — is in the
  // derivation's scope).
  ASSERT_TRUE(cat.ContextFor("Asmodeus").has_value());
}

// The authored override list is non-empty and every row names a real
// descriptor command.
IN_PROC_BROWSER_TEST_F(AurelianSessionContextBrowserTest,
                       OverridesNameDescriptorCommands) {
  const CdpCatalog& cat = CdpCatalog::Get();
  std::vector<std::string> overrides = cat.OverrideCommands();
  ASSERT_FALSE(overrides.empty());
  for (const std::string& qualified : overrides) {
    const size_t dot = qualified.find('.');
    ASSERT_NE(dot, std::string::npos) << qualified;
    EXPECT_NE(cat.FindCommand(qualified.substr(0, dot),
                              qualified.substr(dot + 1)),
              nullptr)
        << "override names a non-descriptor command: " << qualified;
  }
  EXPECT_EQ(cat.OverrideContextFor("SystemInfo.getInfo").value_or(""),
            "browser-only");
}

// LIVE PIN — every derived browser-only row must be refused by the
// HOST on a page session (method-not-found, JSON-RPC -32601). A
// failing pin means the derivation is wrong for that domain (a page
// session serves it via a surface the grep cannot see) and the domain
// moves to renderer_served.json — never the reverse (design section 2
// round-8). If the derived set is empty, that emptiness is asserted
// explicitly (the plan's vacuous-case discipline).
IN_PROC_BROWSER_TEST_F(AurelianSessionContextBrowserTest,
                       BrowserOnlyRowsRefusedOnPageSession) {
  NavigateToFixture();
  const CdpCatalog& cat = CdpCatalog::Get();
  std::vector<std::string> browser_only = cat.BrowserOnlyDomains();
  if (browser_only.empty()) {
    // Explicitly recorded vacuous case — the domain-level refusal RED
    // is then carried by the command-level override pins below.
    SUCCEED() << "derived browser-only set is empty (recorded)";
    return;
  }
  for (const std::string& domain : browser_only) {
    std::optional<std::string> cmd = cat.FirstCommandOf(domain);
    ASSERT_TRUE(cmd.has_value()) << domain << " has no commands";
    const std::string method = domain + "." + *cmd;
    std::string resp = SendCdpCommand(GetWC(), method, "{}");
    EXPECT_NE(resp.find("-32601"), std::string::npos)
        << "browser-only row FALSIFIED by the host: page session served "
        << method << " (move " << domain
        << " into renderer_served.json and regenerate): " << resp;
  }
}

// LIVE PIN — the authored override rows match the host's own gates:
// SystemInfo.getInfo refuses on a page session with the in-handler
// guard's error (NOT method-not-found — the handler IS registered).
IN_PROC_BROWSER_TEST_F(AurelianSessionContextBrowserTest,
                       OverrideGetInfoRefusedOnPageSession) {
  NavigateToFixture();
  std::string resp = SendCdpCommand(GetWC(), "SystemInfo.getInfo", "{}");
  EXPECT_EQ(resp.find("-32601"), std::string::npos)
      << "getInfo was method-not-found; expected the in-handler guard: "
      << resp;
  EXPECT_NE(resp.find("browser target"), std::string::npos)
      << "host gate drifted from the override row: " << resp;
}

IN_PROC_BROWSER_TEST_F(AurelianSessionContextBrowserTest,
                       OverrideGetProcessInfoRefusedOnPageSession) {
  NavigateToFixture();
  std::string resp =
      SendCdpCommand(GetWC(), "SystemInfo.getProcessInfo", "{}");
  EXPECT_EQ(resp.find("-32601"), std::string::npos) << resp;
  EXPECT_NE(resp.find("browser target"), std::string::npos)
      << "host gate drifted from the override row: " << resp;
}

// LIVE PIN — the un-overridden sibling DISPATCHES on a page session
// (the mirror must be exactly as wide as the host): getFeatureState has
// no in-handler session gate, and the chrome layer answers "DIPS"
// definitively. This pin also proves chrome-layer handlers attach to
// embedder-created sessions — if it fails with -32601, that is a
// DESIGN-level finding (chrome-layer rows would not apply to the
// mirror's sessions), not a row to flip.
IN_PROC_BROWSER_TEST_F(AurelianSessionContextBrowserTest,
                       GetFeatureStateDispatchesOnPageSession) {
  NavigateToFixture();
  std::string resp = SendCdpCommand(GetWC(), "SystemInfo.getFeatureState",
                                    "{\"featureState\":\"DIPS\"}");
  EXPECT_NE(resp.find("featureEnabled"), std::string::npos)
      << "getFeatureState did not dispatch on a page session: " << resp;
}

}  // namespace aurelian
