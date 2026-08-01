// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// ACM-5 browser tests (AURELIAN-GENERIC-CONTROL-TDD-PLAN Phase B): the
// preferences mirror — legion://chrome/prefs — the SECOND host registry
// the mirror projects (design section 2 tier table: the prefs mirror via
// PrefService's own registry; names, types, defaults all the HOST's, no
// hand-authored surface). In-process probes against the live profile:
//
//  - `prefs` __getChildren enumerates the registry (several hundred
//    names — the real registered set, not a curated list).
//  - prefs/<name> __getSchema carries the REGISTRY's type + default.
//  - set then get round-trips the named inert pref
//    browser.show_home_button (kShowHomeButton) on the LIVE profile —
//    verified against an independent read of the same source of truth —
//    then restores.
//  - A type-mismatched set is a typed refusal (the host's own
//    SetUserPrefValue NOTREACHED-crashes on mismatch — the mirror gates
//    BEFORE the host, never a crash); an unknown name is typed.
//  - This slice's EmbodimentPolicy row seals `prefs` like every mount.
//
// RED: nothing mounts `prefs` — every dispatch answers
// broken:out-of-scope.

#include <string>

#include "aurelian/handles/root/root_handle.h"
#include "aurelian/membrane/embodiment_policy.h"
#include "base/json/json_reader.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/common/pref_names.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "components/prefs/pref_service.h"
#include "content/public/test/browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace aurelian {

namespace {

std::optional<base::ListValue> ParseList(const std::string& reply) {
  return base::JSONReader::ReadList(reply, base::JSON_PARSE_RFC);
}

std::optional<base::DictValue> ParseDict(const std::string& reply) {
  return base::JSONReader::ReadDict(reply, base::JSON_PARSE_RFC);
}

constexpr char kPrefsPrefix[] = "legion://chrome/prefs/";
constexpr char kInertPref[] = "browser.show_home_button";  // == kShowHomeButton

}  // namespace

class AurelianPrefsMirrorBrowserTest : public InProcessBrowserTest {
 protected:
  PrefService* LivePrefs() { return browser()->profile()->GetPrefs(); }
};

// The registry enumerates — several hundred real names, every child URI
// under the mirror, the named inert pref among them.
IN_PROC_BROWSER_TEST_F(AurelianPrefsMirrorBrowserTest,
                       RegistryEnumeratesHundredsOfNames) {
  ChromeRoot* root = CreateChromeRoot();
  ASSERT_NE(root, nullptr);

  EXPECT_EQ(RootDispatch(root, "prefs/__getIdentity").reply.payload,
            "\"legion://chrome/prefs\"");

  std::string reply = RootDispatch(root, "prefs/__getChildren").reply.payload;
  std::optional<base::ListValue> children = ParseList(reply);
  ASSERT_TRUE(children.has_value()) << "not a JSON list: " << reply;
  EXPECT_GE(children->size(), 300u) << "registry suspiciously small";

  bool saw_inert = false;
  for (const base::Value& c : *children) {
    ASSERT_TRUE(c.is_string()) << reply;
    EXPECT_EQ(c.GetString().rfind(kPrefsPrefix, 0), 0u) << c.GetString();
    if (c.GetString() == std::string(kPrefsPrefix) + kInertPref) {
      saw_inert = true;
    }
  }
  EXPECT_TRUE(saw_inert) << kInertPref << " not enumerated";

  DestroyChromeRoot(root);
}

// __getSchema carries the REGISTRY's type + default (the host's own
// registration, not an authored row), and the node is typed.
IN_PROC_BROWSER_TEST_F(AurelianPrefsMirrorBrowserTest,
                       SchemaCarriesRegistryTypeAndDefault) {
  ChromeRoot* root = CreateChromeRoot();
  ASSERT_NE(root, nullptr);

  EXPECT_EQ(
      RootDispatch(root, std::string("prefs/") + kInertPref + "/__getType")
          .reply.payload,
      "\"legion://types/ChromePreference\"");

  std::string reply =
      RootDispatch(root, std::string("prefs/") + kInertPref + "/__getSchema")
          .reply.payload;
  std::optional<base::DictValue> schema = ParseDict(reply);
  ASSERT_TRUE(schema.has_value()) << reply;
  EXPECT_EQ(*schema->FindString("name"), kInertPref) << reply;
  EXPECT_EQ(*schema->FindString("type"), "boolean") << reply;
  // The default is the registry's: present and boolean-typed (its value
  // is the host's business).
  ASSERT_TRUE(schema->contains("default")) << reply;
  EXPECT_TRUE(schema->FindBool("default").has_value()) << reply;

  // Unknown names are typed, never silence.
  EXPECT_TRUE(RootDispatch(root, "prefs/no.such.pref/__getSchema").reply.is_broken());
  EXPECT_EQ(RootDispatch(root, "prefs/no.such.pref/__getSchema").reply.payload, "unknown-pref");

  DestroyChromeRoot(root);
}

// set then get round-trips on the LIVE profile — verified against an
// independent read of the same source of truth — then restores.
IN_PROC_BROWSER_TEST_F(AurelianPrefsMirrorBrowserTest,
                       SetGetRoundTripsOnLiveProfileThenRestores) {
  ChromeRoot* root = CreateChromeRoot();
  ASSERT_NE(root, nullptr);
  const std::string get_path = std::string("prefs/") + kInertPref + "/get";
  const std::string set_path = std::string("prefs/") + kInertPref + "/set";

  const bool original = LivePrefs()->GetBoolean(prefs::kShowHomeButton);
  EXPECT_EQ(RootDispatch(root, get_path).reply.payload, original ? "true" : "false");

  // Flip through the mirror; the LIVE profile observes it.
  const bool flipped = !original;
  std::string set_reply = RootDispatch(
      root, set_path, std::string("{\"value\":") + (flipped ? "true" : "false") + "}")
      .reply.payload;
  EXPECT_EQ(set_reply, flipped ? "true" : "false") << set_reply;
  EXPECT_EQ(LivePrefs()->GetBoolean(prefs::kShowHomeButton), flipped)
      << "the live profile did not observe the mirror's write";
  EXPECT_EQ(RootDispatch(root, get_path).reply.payload, flipped ? "true" : "false");

  // Restore.
  RootDispatch(root, set_path,
               std::string("{\"value\":") + (original ? "true" : "false") + "}");
  EXPECT_EQ(LivePrefs()->GetBoolean(prefs::kShowHomeButton), original);
  EXPECT_EQ(RootDispatch(root, get_path).reply.payload, original ? "true" : "false");

  DestroyChromeRoot(root);
}

// A type-mismatched set is a TYPED refusal gated by the mirror — the
// host's own write path NOTREACHED-crashes on mismatch, so passing it
// through is not an option — and the live value is untouched.
IN_PROC_BROWSER_TEST_F(AurelianPrefsMirrorBrowserTest,
                       TypeMismatchedSetRefusedTyped) {
  ChromeRoot* root = CreateChromeRoot();
  ASSERT_NE(root, nullptr);

  const bool original = LivePrefs()->GetBoolean(prefs::kShowHomeButton);
  std::string reply =
      RootDispatch(root, std::string("prefs/") + kInertPref + "/set",
                   "{\"value\":\"not-a-bool\"}")
          .reply.payload;
  EXPECT_EQ(reply.rfind("broken:pref-type-mismatch", 0), 0u) << reply;
  EXPECT_EQ(LivePrefs()->GetBoolean(prefs::kShowHomeButton), original);

  // A set with no value field is typed too.
  EXPECT_TRUE(RootDispatch(root, std::string("prefs/") + kInertPref + "/set",
                           "{}")
                  .reply.is_broken());
  EXPECT_EQ(RootDispatch(root, std::string("prefs/") + kInertPref + "/set",
                         "{}")
                .reply.payload,
            "set-needs-value");

  DestroyChromeRoot(root);
}

// This slice's EmbodimentPolicy row (per-slice policy growth, design
// section 7 review N4): `prefs` is sealed behind the membrane.
IN_PROC_BROWSER_TEST_F(AurelianPrefsMirrorBrowserTest,
                       PrefsSealedBehindPolicy) {
  ChromeRoot* sealed_off = CreateChromeRootWithPolicy(
      EmbodimentPolicy::WithCapabilities({"system"}));
  ASSERT_NE(sealed_off, nullptr);
  EXPECT_TRUE(RootDispatch(sealed_off, "prefs").reply.is_broken());
  DestroyChromeRoot(sealed_off);

  ChromeRoot* full = CreateChromeRoot();
  ASSERT_NE(full, nullptr);
  std::string reply = RootDispatch(full, "prefs/__getChildren").reply.payload;
  EXPECT_TRUE(ParseList(reply).has_value())
      << "FullStandalone does not mount prefs: " << reply;
  DestroyChromeRoot(full);
}

}  // namespace aurelian
