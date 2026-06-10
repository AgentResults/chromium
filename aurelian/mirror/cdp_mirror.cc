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

// The declared Layer-1 type URIs the projector stamps (design section 6;
// declared in the ontology at ACM-7, where the consistency audit locks
// every stamp to a declaration).
constexpr char kTypeCatalog[] = "legion://types/CdpCatalog";
constexpr char kTypeDomain[] = "legion://types/CdpDomain";
constexpr char kTypeCommand[] = "legion://types/CdpCommand";
constexpr char kTypeEvent[] = "legion://types/CdpEvent";

// The ONE mirror node class (design invariant: data-driven, NO per-domain
// code). A node's kind + path segments fully determine its answers; every
// answer is projected from CdpCatalog::Get() at ask time.
class CdpMirrorNode : public Handle {
 public:
  enum class Kind { kCatalog, kDomain, kCommand, kEvent };

  CdpMirrorNode(Kind kind, std::string domain, std::string member)
      : kind_(kind),
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
    // Reserved / unwrap probes never fall through to catalog lookup.
    if (!msg.empty() && msg.front() == '_') {
      return ValueHandle::make_broken("unknown-message");
    }
    return Child(std::string(msg));
  }

  void tell(std::string_view, const Value&) override {}

 private:
  std::string MakeUri() const {
    std::string uri = kMirrorUri;
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
        std::optional<CdpCatalog::ContextRow> row = cat.ContextFor(domain_);
        if (row.has_value()) {
          fields.emplace("sessionContext",
                         Value::make_object({
                             {"inBrowserUnion", Value(row->in_browser_union)},
                             {"browserOnly", Value(row->browser_only)},
                             {"conditional", Value(row->conditional)},
                         }));
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

  // ACM-2: invoke through the HS-1 session layer, gated by the
  // session-context rule (design section 2, NORMATIVE): the browser
  // session's width is CLOSED — a domain absent from the derived closed
  // union is the typed refusal NAMING the per-target path, never a raw CDP
  // "wasn't found" passthrough. In-union commands dispatch; the host's own
  // answer (result or error) passes through. The rows are ACM-S2's derived
  // table — nothing here is hand-named.
  std::shared_ptr<Handle> Invoke(const Value& spec) const {
    const CdpCatalog& cat = CdpCatalog::Get();
    std::optional<CdpCatalog::ContextRow> row = cat.ContextFor(domain_);
    if (!row.has_value() || !row->in_browser_union) {
      return ValueHandle::make_broken(
          "session-context: " + domain_ + "." + member_ +
          " is not in the browser session's closed union; use targets/<id>/cdp/" +
          domain_ + "/" + member_);
    }
    return CdpSessionRegistry::Get().InvokeOnBrowserTarget(
        domain_ + "." + member_, spec);
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
        return std::make_shared<CdpMirrorNode>(Kind::kDomain, name,
                                               std::string());
      case Kind::kDomain:
        // No command/event name collides anywhere in the descriptor
        // (verified at ACM-1), so command-first lookup is total.
        if (cat.FindCommand(domain_, name)) {
          return std::make_shared<CdpMirrorNode>(Kind::kCommand, domain_,
                                                 name);
        }
        if (cat.FindEvent(domain_, name)) {
          return std::make_shared<CdpMirrorNode>(Kind::kEvent, domain_, name);
        }
        return ValueHandle::make_broken("unknown-command-or-event");
      case Kind::kCommand:
      case Kind::kEvent:
        return ValueHandle::make_broken("unknown-message");
    }
    return ValueHandle::make_broken("unknown-message");
  }

  const Kind kind_;
  const std::string domain_;
  const std::string member_;
  const Value identity_;
};

}  // namespace

std::shared_ptr<velite::agentspaces::Handle> CreateCdpMirror() {
  return std::make_shared<CdpMirrorNode>(CdpMirrorNode::Kind::kCatalog,
                                         std::string(), std::string());
}

}  // namespace aurelian
