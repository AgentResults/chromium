// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// ACM-6 browser tests (AURELIAN-GENERIC-CONTROL-TDD-PLAN Phase B): the
// services catalog — legion://chrome/services — CATALOG-ONLY (design
// sections 2/6/8: C++ has no runtime method reflection, so Tier-2 invoke
// does NOT transfer; the honesty is load-bearing and asserted). The
// catalog is the KeyedServiceFactory dependency graph, enumerated via
// DependencyGraph::GetConstructionOrder over the ONE-LINE additive
// production accessor on DependencyManager (the recorded fork-delta
// line — GetDependencyGraphForTesting is not usable in prod).
//
// The enumeration assert is a round-trip against an INDEPENDENT read of
// the same source of truth: the test walks the graph itself (via the
// ForTesting accessor — test-only use is fine, design section 8) and
// the mirror's children must match it exactly.
//
// RED: nothing mounts `services` — every dispatch answers
// broken:out-of-scope.

#include <set>
#include <string>

#include "aurelian/handles/root/root_handle.h"
#include "aurelian/membrane/embodiment_policy.h"
#include "base/json/json_reader.h"
#include "base/memory/raw_ptr.h"
#include "base/values.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "components/keyed_service/content/browser_context_dependency_manager.h"
#include "components/keyed_service/core/dependency_graph.h"
#include "components/keyed_service/core/keyed_service_base_factory.h"
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

constexpr char kServicesPrefix[] = "legion://chrome/services/";

// The INDEPENDENT read of the same source of truth (test tier; the
// mirror's production path is the additive accessor).
std::set<std::string> GraphNamesDirect() {
  std::vector<raw_ptr<DependencyNode, VectorExperimental>> order;
  if (!BrowserContextDependencyManager::GetInstance()
           ->GetDependencyGraphForTesting()
           .GetConstructionOrder(&order)) {
    return {};
  }
  std::set<std::string> names;
  for (DependencyNode* node : order) {
    names.insert(static_cast<KeyedServiceBaseFactory*>(node)->name());
  }
  return names;
}

}  // namespace

class AurelianServicesMirrorBrowserTest : public InProcessBrowserTest {};

// The catalog enumerates the REAL factory graph — exactly the host's own
// registered set, dozens-to-hundreds strong, no curation.
IN_PROC_BROWSER_TEST_F(AurelianServicesMirrorBrowserTest,
                       CatalogMatchesTheFactoryGraphExactly) {
  ChromeRoot* root = CreateChromeRoot();
  ASSERT_NE(root, nullptr);

  EXPECT_EQ(RootDispatch(root, "services/__getIdentity").reply.payload,
            "\"legion://chrome/services\"");

  std::string reply = RootDispatch(root, "services/__getChildren").reply.payload;
  std::optional<base::ListValue> children = ParseList(reply);
  ASSERT_TRUE(children.has_value()) << "not a JSON list: " << reply;

  std::set<std::string> direct = GraphNamesDirect();
  ASSERT_GE(direct.size(), 50u) << "factory graph suspiciously small";

  std::set<std::string> mirrored;
  for (const base::Value& c : *children) {
    ASSERT_TRUE(c.is_string()) << reply;
    ASSERT_EQ(c.GetString().rfind(kServicesPrefix, 0), 0u) << c.GetString();
    mirrored.insert(c.GetString().substr(std::string(kServicesPrefix).size()));
  }
  EXPECT_EQ(mirrored, direct)
      << "the mirror's catalog does not match the host's own graph";

  DestroyChromeRoot(root);
}

// Nodes are typed; invoke is the TYPED no-invoke-surface refusal (the
// honesty is load-bearing: Tier-2 invoke does not transfer to C++ —
// asserted, never silent); unknown names are typed.
IN_PROC_BROWSER_TEST_F(AurelianServicesMirrorBrowserTest,
                       NodesTypedAndInvokeRefusedHonestly) {
  ChromeRoot* root = CreateChromeRoot();
  ASSERT_NE(root, nullptr);

  std::set<std::string> direct = GraphNamesDirect();
  ASSERT_FALSE(direct.empty());
  const std::string name = *direct.begin();

  EXPECT_EQ(RootDispatch(root, "services/" + name + "/__getType").reply.payload,
            "\"legion://types/ChromeService\"");

  std::string schema_reply =
      RootDispatch(root, "services/" + name + "/__getSchema").reply.payload;
  std::optional<base::DictValue> schema = ParseDict(schema_reply);
  ASSERT_TRUE(schema.has_value()) << schema_reply;
  EXPECT_EQ(*schema->FindString("name"), name) << schema_reply;
  EXPECT_EQ(schema->FindBool("catalogOnly").value_or(false), true)
      << schema_reply;

  WireReply invoke_reply =
      RootDispatch(root, "services/" + name + "/invoke").reply;
  EXPECT_TRUE(invoke_reply.is_broken()) << invoke_reply;
  EXPECT_EQ(invoke_reply.payload, "no-invoke-surface") << invoke_reply;

  EXPECT_TRUE(RootDispatch(root, "services/NoSuchService/__getType").reply.is_broken());
  EXPECT_EQ(RootDispatch(root, "services/NoSuchService/__getType").reply.payload, "unknown-service");

  DestroyChromeRoot(root);
}

// This slice's EmbodimentPolicy row: `services` is sealed behind the
// membrane like every mount.
IN_PROC_BROWSER_TEST_F(AurelianServicesMirrorBrowserTest,
                       ServicesSealedBehindPolicy) {
  ChromeRoot* sealed_off = CreateChromeRootWithPolicy(
      EmbodimentPolicy::WithCapabilities({"system"}));
  ASSERT_NE(sealed_off, nullptr);
  EXPECT_TRUE(RootDispatch(sealed_off, "services").reply.is_broken());
  DestroyChromeRoot(sealed_off);

  ChromeRoot* full = CreateChromeRoot();
  ASSERT_NE(full, nullptr);
  std::string reply = RootDispatch(full, "services/__getChildren").reply.payload;
  EXPECT_TRUE(ParseList(reply).has_value())
      << "FullStandalone does not mount services: " << reply;
  DestroyChromeRoot(full);
}

}  // namespace aurelian
