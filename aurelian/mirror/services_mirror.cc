// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/mirror/services_mirror.h"

#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "components/keyed_service/content/browser_context_dependency_manager.h"
#include "components/keyed_service/core/dependency_graph.h"
#include "components/keyed_service/core/keyed_service_base_factory.h"
#include "velite/agentspaces-wire/handle.hpp"
#include "velite/agentspaces-wire/value_handle.hpp"

namespace aurelian {

namespace {

using velite::agentspaces::Handle;
using velite::agentspaces::StateKind;
using velite::agentspaces::Value;
using velite::agentspaces::ValueHandle;

constexpr char kServicesUri[] = "legion://chrome/services";

// The declared Layer-1 type URIs (declared in the ontology at ACM-7,
// including the collection-root type — Phase-A review F2).
constexpr char kTypeServices[] = "legion://types/ChromeServices";
constexpr char kTypeService[] = "legion://types/ChromeService";

// The registered factory names, via the production accessor (the
// section-7 fork-delta line) — every node in the graph is a
// KeyedServiceBaseFactory (DependencyManager::AddComponent is the only
// insertion path).
std::set<std::string> FactoryNames() {
  std::vector<raw_ptr<DependencyNode, VectorExperimental>> order;
  if (!BrowserContextDependencyManager::GetInstance()
           ->GetDependencyGraph()
           .GetConstructionOrder(&order)) {
    return {};
  }
  std::set<std::string> names;
  for (DependencyNode* node : order) {
    names.insert(static_cast<KeyedServiceBaseFactory*>(node)->name());
  }
  return names;
}

// legion://chrome/services/<name> — ONE catalog entry. Catalog-only:
// invoke is the typed refusal (design section 8 — no Tier-2 invoke in
// C++; the refusal IS the surface, never silence).
class ServiceNode : public Handle {
 public:
  explicit ServiceNode(std::string name)
      : name_(std::move(name)),
        identity_(std::string(kServicesUri) + "/" + name_) {}

  StateKind state_kind() const override { return StateKind::ResolvedValue; }
  const Value& resolved_value() const override { return identity_; }
  std::shared_ptr<Handle> resolved_handle() const override { return nullptr; }
  std::string_view broken_reason() const override { return ""; }
  std::string sturdy_identity() const override {
    return identity_.as_string();
  }

  std::shared_ptr<Handle> ask_impl(std::string_view msg,
                                   const Value& /*spec*/) override {
    if (msg == "__getIdentity") {
      return ValueHandle::make(identity_);
    }
    if (msg == "__getType") {
      return ValueHandle::make(Value(std::string(kTypeService)));
    }
    if (msg == "__getSchema") {
      return ValueHandle::make(Value::make_object({
          {"name", Value(name_)},
          {"catalogOnly", Value(true)},
      }));
    }
    if (msg == "invoke") {
      return ValueHandle::make_broken(
          "no-invoke-surface: C++ has no runtime method reflection; the "
          "services catalog is enumeration-only (design section 8)");
    }
    return ValueHandle::make_broken("unknown-message");
  }

  void tell(std::string_view, const Value&) override {}

 private:
  const std::string name_;
  const Value identity_;
};

// legion://chrome/services — the catalog root. Every __getChildren is a
// fresh walk of the live dependency graph.
class ServicesMirrorNode : public Handle {
 public:
  ServicesMirrorNode() = default;

  StateKind state_kind() const override { return StateKind::ResolvedValue; }
  const Value& resolved_value() const override { return identity_; }
  std::shared_ptr<Handle> resolved_handle() const override { return nullptr; }
  std::string_view broken_reason() const override { return ""; }
  std::string sturdy_identity() const override { return kServicesUri; }

  std::shared_ptr<Handle> ask_impl(std::string_view msg,
                                   const Value& /*spec*/) override {
    if (msg == "__getIdentity") {
      return ValueHandle::make(identity_);
    }
    if (msg == "__getType") {
      return ValueHandle::make(Value(std::string(kTypeServices)));
    }
    if (msg == "__getChildren") {
      std::vector<Value> children;
      for (const std::string& name : FactoryNames()) {
        children.emplace_back(std::string(kServicesUri) + "/" + name);
      }
      return ValueHandle::make(Value::make_array(std::move(children)));
    }
    if (msg == "__getSchema") {
      return ValueHandle::make(Value::make_object({
          {"count", Value(static_cast<int64_t>(FactoryNames().size()))},
          {"catalogOnly", Value(true)},
      }));
    }
    if (!msg.empty() && msg.front() == '_') {
      return ValueHandle::make_broken("unknown-message");
    }
    if (!FactoryNames().count(std::string(msg))) {
      return ValueHandle::make_broken("unknown-service");
    }
    return std::make_shared<ServiceNode>(std::string(msg));
  }

  void tell(std::string_view, const Value&) override {}

 private:
  Value identity_{std::string(kServicesUri)};
};

}  // namespace

std::shared_ptr<velite::agentspaces::Handle> CreateServicesMirror() {
  return std::make_shared<ServicesMirrorNode>();
}

}  // namespace aurelian
