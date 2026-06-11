// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/mirror/targets_mirror.h"

#include <string>
#include <utility>
#include <vector>

#include "aurelian/mirror/cdp_mirror.h"
#include "base/memory/scoped_refptr.h"
#include "content/public/browser/devtools_agent_host.h"
#include "url/gurl.h"
#include "velite/agentspaces-wire/handle.hpp"
#include "velite/agentspaces-wire/value_handle.hpp"

namespace aurelian {

namespace {

using velite::agentspaces::Handle;
using velite::agentspaces::StateKind;
using velite::agentspaces::Value;
using velite::agentspaces::ValueHandle;

constexpr char kTargetsUri[] = "legion://chrome/targets";

// The declared Layer-1 type URIs (design section 6; declared in the
// ontology at ACM-7, where the consistency audit locks every stamp).
constexpr char kTypeTargets[] = "legion://types/ChromeTargets";
constexpr char kTypeTarget[] = "legion://types/ChromeTarget";

// legion://chrome/targets/<id> — ONE live target, typed. Minted per ask
// (stateless projection — the per-target session state lives in the
// ACM-2/3 session registry, the design-section-3 residency rule); the
// node holds its host ref only for the duration of the ask chain.
class TargetNode : public Handle {
 public:
  explicit TargetNode(scoped_refptr<content::DevToolsAgentHost> host)
      : host_(std::move(host)),
        identity_(std::string(kTargetsUri) + "/" + host_->GetId()) {}

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
      return ValueHandle::make(Value(std::string(kTypeTarget)));
    }
    if (msg == "__getChildren") {
      return ValueHandle::make(Value::make_array(
          {Value(identity_.as_string() + "/cdp")}));
    }
    if (msg == "__getSchema") {
      // The CDP target type, first-class (design section 4: the mirror
      // does not pretend the target set is "tabs"; tab-shaped asserts
      // filter on `page`).
      return ValueHandle::make(Value::make_object({
          {"id", Value(host_->GetId())},
          {"type", Value(host_->GetType())},
          {"url", Value(host_->GetURL().spec())},
          {"title", Value(host_->GetTitle())},
          {"attached", Value(host_->IsAttached())},
      }));
    }
    if (msg == "cdp") {
      return CreateCdpMirrorForTarget(host_->GetId(), host_->GetType());
    }
    // No leaf handle exposes an unwrap path to its ambient reference.
    return ValueHandle::make_broken("unknown-message");
  }

  void tell(std::string_view, const Value&) override {}

 private:
  const scoped_refptr<content::DevToolsAgentHost> host_;
  const Value identity_;
};

// legion://chrome/targets — the live enumeration. Every __getChildren is
// a fresh GetOrCreateAll() (the catalog grows and shrinks with the real
// target set; nothing is cached); navigation by id resolves via GetForId
// (hosts self-retain while their entity lives, so every id the
// enumeration minted resolves until its target closes).
class TargetsMirrorNode : public Handle {
 public:
  TargetsMirrorNode() = default;

  StateKind state_kind() const override { return StateKind::ResolvedValue; }
  const Value& resolved_value() const override { return identity_; }
  std::shared_ptr<Handle> resolved_handle() const override { return nullptr; }
  std::string_view broken_reason() const override { return ""; }
  std::string sturdy_identity() const override { return kTargetsUri; }

  std::shared_ptr<Handle> ask_impl(std::string_view msg,
                                   const Value& /*spec*/) override {
    if (msg == "__getIdentity") {
      return ValueHandle::make(identity_);
    }
    if (msg == "__getType") {
      return ValueHandle::make(Value(std::string(kTypeTargets)));
    }
    if (msg == "__getChildren") {
      std::vector<Value> children;
      for (const scoped_refptr<content::DevToolsAgentHost>& host :
           content::DevToolsAgentHost::GetOrCreateAll()) {
        children.emplace_back(std::string(kTargetsUri) + "/" + host->GetId());
      }
      return ValueHandle::make(Value::make_array(std::move(children)));
    }
    if (msg == "__getSchema") {
      return ValueHandle::make(Value::make_object({
          {"count",
           Value(static_cast<int64_t>(
               content::DevToolsAgentHost::GetOrCreateAll().size()))},
      }));
    }
    if (!msg.empty() && msg.front() == '_') {
      return ValueHandle::make_broken("unknown-message");
    }
    scoped_refptr<content::DevToolsAgentHost> host =
        content::DevToolsAgentHost::GetForId(std::string(msg));
    if (!host) {
      return ValueHandle::make_broken("unknown-target");
    }
    return std::make_shared<TargetNode>(std::move(host));
  }

  void tell(std::string_view, const Value&) override {}

 private:
  Value identity_{std::string(kTargetsUri)};
};

}  // namespace

std::shared_ptr<velite::agentspaces::Handle> CreateTargetsMirror() {
  return std::make_shared<TargetsMirrorNode>();
}

}  // namespace aurelian
