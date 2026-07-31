// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/mirror/cdp_mirror.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "aurelian/catalog/cdp_catalog.h"
#include "aurelian/mirror/cdp_session.h"
#include "aurelian/mirror/value_convert.h"
#include "base/values.h"
#include "velite/agentspaces-wire/handle.hpp"
#include "velite/agentspaces-wire/value_handle.hpp"

namespace aurelian {

namespace {

using velite::agentspaces::Handle;
using velite::agentspaces::StateKind;
using velite::agentspaces::Value;
using velite::agentspaces::ValueHandle;

constexpr char kMirrorUri[] = "legion://chrome/cdp";
constexpr char kTargetsUri[] = "legion://chrome/targets";

// The width model applies to the browser mirror and PAGE/FRAME target
// sub-mirrors only (design section 2 round-8); every other target type is
// annotation-not-applicable and dispatch-and-pass-through.
bool IsPageOrFrameType(const std::string& target_type) {
  return target_type == "page" || target_type == "iframe";
}

// The declared Layer-1 type URIs the projector stamps (design section 6;
// declared in the ontology at ACM-7, where the consistency audit locks
// every stamp to a declaration).
constexpr char kTypeCatalog[] = "legion://types/CdpCatalog";
constexpr char kTypeDomain[] = "legion://types/CdpDomain";
constexpr char kTypeCommand[] = "legion://types/CdpCommand";
constexpr char kTypeEvent[] = "legion://types/CdpEvent";

// The ONE mirror node class (design invariant: data-driven, NO per-domain
// code). A node's kind + scope + path segments fully determine its
// answers; every answer is projected from CdpCatalog::Get() at ask time.
// Scope (ACM-3): an empty target id is the BROWSER mirror
// (legion://chrome/cdp); a non-empty one is THAT target's sub-mirror
// (legion://chrome/targets/<id>/cdp), whose invokes dispatch on the
// target's persistent session behind the per-scope session-context gate.
class CdpMirrorNode : public Handle {
 public:
  enum class Kind { kCatalog, kDomain, kCommand, kEvent };

  struct Scope {
    std::string target_id;    // empty = the browser mirror
    std::string target_type;  // DevToolsAgentHost::GetType()'s string
  };

  CdpMirrorNode(Kind kind, Scope scope, std::string domain, std::string member)
      : kind_(kind),
        scope_(std::move(scope)),
        domain_(std::move(domain)),
        member_(std::move(member)),
        identity_(MakeUri()) {}

  StateKind state_kind() const override { return StateKind::ResolvedValue; }
  const Value& resolved_value() const override { return identity_; }
  std::shared_ptr<Handle> resolved_handle() const override { return nullptr; }
  std::string_view broken_reason() const override { return ""; }
  std::string sturdy_identity() const override { return uri(); }

  std::shared_ptr<Handle> ask_impl(std::string_view msg,
                                   const Value& spec) override {
    if (msg == "__getIdentity") {
      return ValueHandle::make(identity_);
    }
    if (msg == "__getType") {
      return ValueHandle::make(Value(std::string(TypeUri())));
    }
    if (msg == "__getChildren") {
      return ValueHandle::make(Children());
    }
    if (msg == "__getSchema") {
      return Schema();
    }
    // ACM-2 (HS-1): invoke on a command node dispatches on the BROWSER
    // target's persistent session — behind the session-context gate.
    if (msg == "invoke" && kind_ == Kind::kCommand) {
      return Invoke(spec);
    }
    // ACM-4: legion-subscribe({sink}) on an event node — a CDP event subscription
    // IS a handle subscription (design section 3), behind the SAME
    // session-context gate as invoke (design section 2 round-4: never
    // accepted-but-silently-event-less).
    //
    // The message is `legion-subscribe`, the NORMATIVE substrate name
    // (spec/facilities/subscribe.md section 1 [SUBSCRIBE-UNIFORM-CALL-SHAPE],
    // spec/foundations/handle.md section 6.4 [HANDLE-LEGION-MESSAGE-FAMILY-RESERVED]).
    // It was previously the bare `subscribe`, which NO spec-conformant caller
    // sends — the MCP bridge, like every other substrate consumer, asks
    // `legion-subscribe`. The mirror therefore never matched, fell through to
    // Child("legion-subscribe"), and the caller was handed a subscription that
    // could never deliver an event (found live 2026-07-31: subscribing to
    // Page.frameNavigated and Page.screencastFrame drained 0 events forever).
    if (msg == "legion-subscribe" && kind_ == Kind::kEvent) {
      return Subscribe(spec);
    }
    // Reserved families never fall through to catalog lookup: `__` probes and
    // the substrate's `legion-` facility messages (handle.md section 6.4). A
    // `legion-*` message this node does not answer is UNKNOWN — resolving it as
    // a child name is what silently turned a failed subscribe into a node.
    if (!msg.empty() && (msg.front() == '_' || msg.rfind("legion-", 0) == 0)) {
      return ValueHandle::make_broken("unknown-message");
    }
    return Child(std::string(msg));
  }

  void tell(std::string_view, const Value&) override {}

 private:
  std::string MakeUri() const {
    std::string uri =
        scope_.target_id.empty()
            ? std::string(kMirrorUri)
            : std::string(kTargetsUri) + "/" + scope_.target_id + "/cdp";
    if (!domain_.empty()) {
      uri += "/" + domain_;
    }
    if (!member_.empty()) {
      uri += "/" + member_;
    }
    return uri;
  }

  std::string uri() const { return identity_.as_string(); }

  const char* TypeUri() const {
    switch (kind_) {
      case Kind::kCatalog:
        return kTypeCatalog;
      case Kind::kDomain:
        return kTypeDomain;
      case Kind::kCommand:
        return kTypeCommand;
      case Kind::kEvent:
        return kTypeEvent;
    }
    return kTypeCatalog;
  }

  // Child URIs, straight from the catalog: domains under the mirror root;
  // commands + events under a domain; none under a leaf.
  Value Children() const {
    const CdpCatalog& cat = CdpCatalog::Get();
    std::vector<Value> children;
    if (kind_ == Kind::kCatalog) {
      for (const std::string& d : cat.domains()) {
        children.emplace_back(uri() + "/" + d);
      }
    } else if (kind_ == Kind::kDomain) {
      for (const std::string& c : cat.CommandsOf(domain_)) {
        children.emplace_back(uri() + "/" + c);
      }
      for (const std::string& e : cat.EventsOf(domain_)) {
        children.emplace_back(uri() + "/" + e);
      }
    }
    return Value::make_array(std::move(children));
  }

  std::shared_ptr<Handle> Schema() const {
    const CdpCatalog& cat = CdpCatalog::Get();
    switch (kind_) {
      case Kind::kCatalog:
        return ValueHandle::make(Value::make_object({
            {"domains", Value(static_cast<int64_t>(cat.domain_count()))},
            {"commands", Value(static_cast<int64_t>(cat.command_count()))},
            {"events", Value(static_cast<int64_t>(cat.event_count()))},
        }));
      case Kind::kDomain: {
        const base::DictValue* dd = cat.FindDomain(domain_);
        if (!dd) {
          return ValueHandle::make_broken("unknown-domain");
        }
        // The descriptor entry minus its bulk member lists (those are the
        // children), plus the DERIVED session-context annotation
        // (design section 2 — the rows are ACM-S2's, never hand-named).
        std::map<std::string, Value> fields;
        for (const auto [key, field] : *dd) {
          if (key == "commands" || key == "events" || key == "types") {
            continue;
          }
          fields.emplace(key, FromBaseValue(field));
        }
        // The derived rows scope the browser mirror + page/frame
        // sub-mirrors ONLY; a non-page sub-mirror's annotation is
        // not-applicable (design section 2 round-8 — its session is
        // narrow and self-describing by answer).
        if (!scope_.target_id.empty() &&
            !IsPageOrFrameType(scope_.target_type)) {
          fields.emplace("sessionContext", Value::make_object({
                                               {"notApplicable", Value(true)},
                                           }));
        } else {
          std::optional<CdpCatalog::ContextRow> row = cat.ContextFor(domain_);
          if (row.has_value()) {
            fields.emplace(
                "sessionContext",
                Value::make_object({
                    {"inBrowserUnion", Value(row->in_browser_union)},
                    {"browserOnly", Value(row->browser_only)},
                    {"conditional", Value(row->conditional)},
                }));
          }
        }
        return ValueHandle::make(Value::make_object(std::move(fields)));
      }
      case Kind::kCommand: {
        const base::DictValue* cd = cat.FindCommand(domain_, member_);
        return cd ? ValueHandle::make(FromBaseDict(*cd))
                  : ValueHandle::make_broken("unknown-command-or-event");
      }
      case Kind::kEvent: {
        const base::DictValue* ed = cat.FindEvent(domain_, member_);
        return ed ? ValueHandle::make(FromBaseDict(*ed))
                  : ValueHandle::make_broken("unknown-command-or-event");
      }
    }
    return ValueHandle::make_broken("unknown-message");
  }

  // ACM-2/ACM-3: invoke through the HS-1 session layer, gated by the
  // session-context rule (design section 2, NORMATIVE) — both directions
  // typed redirects NAMING the correct path, never a raw CDP "wasn't
  // found" passthrough from the wrong session. The rows are ACM-S2's
  // derived table + the authored per-command overrides — nothing here is
  // hand-named.
  //  - Browser mirror: the width is CLOSED — a domain absent from the
  //    derived union refuses naming targets/<id>/cdp/...
  //  - Page/frame sub-mirror: the width is OPEN — only the DERIVED closed
  //    browser-only set and the authored command overrides refuse (naming
  //    the browser path cdp/...); everything else dispatches and the
  //    host's own answer (result or error) passes through.
  //  - Non-page sub-mirror: dispatch-and-pass-through, NO derived
  //    refusals (round-8 — the session is self-describing by answer).
  std::shared_ptr<Handle> Invoke(const Value& spec) const {
    const CdpCatalog& cat = CdpCatalog::Get();
    const std::string method = domain_ + "." + member_;
    if (scope_.target_id.empty()) {
      std::optional<CdpCatalog::ContextRow> row = cat.ContextFor(domain_);
      if (!row.has_value() || !row->in_browser_union) {
        return ValueHandle::make_broken(
            "session-context: " + method +
            " is not in the browser session's closed union; use "
            "targets/<id>/cdp/" +
            domain_ + "/" + member_);
      }
      return CdpSessionRegistry::Get().InvokeOnBrowserTarget(method, spec);
    }
    if (IsPageOrFrameType(scope_.target_type)) {
      std::optional<CdpCatalog::ContextRow> row = cat.ContextFor(domain_);
      if (row.has_value() && row->browser_only) {
        return ValueHandle::make_broken(
            "session-context: " + method + " is browser-only; use cdp/" +
            domain_ + "/" + member_);
      }
      if (cat.OverrideContextFor(method).value_or("") == "browser-only") {
        return ValueHandle::make_broken(
            "session-context: " + method +
            " is browser-only by per-command override; use cdp/" + domain_ +
            "/" + member_);
      }
    }
    return CdpSessionRegistry::Get().InvokeOnTarget(scope_.target_id, method,
                                                    spec);
  }

  // ACM-4: the subscription half of the session-context rule (design
  // section 2 round-4) — the same gate shape as Invoke, both directions
  // typed redirects naming the correct path (a page-scoped event at the
  // browser mirror; a derived-browser-only event at a page sub-mirror);
  // non-page sub-mirrors pass through. Command-level overrides are
  // command-granular and do not apply to events.
  std::shared_ptr<Handle> Subscribe(const Value& spec) const {
    const Value* sink = spec.object_get("sink");
    if (!sink || !sink->is_handle() || !sink->as_handle()) {
      return ValueHandle::make_broken("subscribe-needs-sink");
    }
    const CdpCatalog& cat = CdpCatalog::Get();
    const std::string event_method = domain_ + "." + member_;
    if (scope_.target_id.empty()) {
      std::optional<CdpCatalog::ContextRow> row = cat.ContextFor(domain_);
      if (!row.has_value() || !row->in_browser_union) {
        return ValueHandle::make_broken(
            "session-context: " + event_method +
            " is not in the browser session's closed union; use "
            "targets/<id>/cdp/" +
            domain_ + "/" + member_);
      }
      return CdpSessionRegistry::Get().SubscribeOnBrowserTarget(
          event_method, sink->as_handle(), uri());
    }
    if (IsPageOrFrameType(scope_.target_type)) {
      std::optional<CdpCatalog::ContextRow> row = cat.ContextFor(domain_);
      if (row.has_value() && row->browser_only) {
        return ValueHandle::make_broken(
            "session-context: " + event_method + " is browser-only; use cdp/" +
            domain_ + "/" + member_);
      }
    }
    return CdpSessionRegistry::Get().SubscribeOnTarget(
        scope_.target_id, event_method, sink->as_handle(), uri());
  }

  // Catalog-lookup navigation. Misses answer TYPED reasons; nodes are
  // minted per ask (stateless projection — per-target session state lives
  // in the ACM-2 session registry, not in these nodes).
  std::shared_ptr<Handle> Child(const std::string& name) const {
    const CdpCatalog& cat = CdpCatalog::Get();
    switch (kind_) {
      case Kind::kCatalog:
        if (!cat.FindDomain(name)) {
          return ValueHandle::make_broken("unknown-domain");
        }
        return std::make_shared<CdpMirrorNode>(Kind::kDomain, scope_, name,
                                               std::string());
      case Kind::kDomain:
        // No command/event name collides anywhere in the descriptor
        // (verified at ACM-1), so command-first lookup is total.
        if (cat.FindCommand(domain_, name)) {
          return std::make_shared<CdpMirrorNode>(Kind::kCommand, scope_,
                                                 domain_, name);
        }
        if (cat.FindEvent(domain_, name)) {
          return std::make_shared<CdpMirrorNode>(Kind::kEvent, scope_, domain_,
                                                 name);
        }
        return ValueHandle::make_broken("unknown-command-or-event");
      case Kind::kCommand:
      case Kind::kEvent:
        return ValueHandle::make_broken("unknown-message");
    }
    return ValueHandle::make_broken("unknown-message");
  }

  const Kind kind_;
  const Scope scope_;
  const std::string domain_;
  const std::string member_;
  const Value identity_;
};

}  // namespace

std::shared_ptr<velite::agentspaces::Handle> CreateCdpMirror() {
  return std::make_shared<CdpMirrorNode>(CdpMirrorNode::Kind::kCatalog,
                                         CdpMirrorNode::Scope(), std::string(),
                                         std::string());
}

std::shared_ptr<velite::agentspaces::Handle> CreateCdpMirrorForTarget(
    const std::string& target_id,
    const std::string& target_type) {
  return std::make_shared<CdpMirrorNode>(
      CdpMirrorNode::Kind::kCatalog,
      CdpMirrorNode::Scope{target_id, target_type}, std::string(),
      std::string());
}

}  // namespace aurelian
