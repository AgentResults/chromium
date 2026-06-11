// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/mirror/prefs_mirror.h"

#include <string>
#include <utility>
#include <vector>

#include "aurelian/mirror/value_convert.h"
#include "base/functional/bind.h"
#include "base/values.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/prefs/pref_service.h"
#include "components/user_prefs/user_prefs.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/web_contents.h"
#include "velite/agentspaces-wire/handle.hpp"
#include "velite/agentspaces-wire/value_handle.hpp"

namespace aurelian {

namespace {

using velite::agentspaces::Handle;
using velite::agentspaces::StateKind;
using velite::agentspaces::Value;
using velite::agentspaces::ValueHandle;

constexpr char kPrefsUri[] = "legion://chrome/prefs";

// The declared Layer-1 type URIs (design section 6; declared in the
// ontology at ACM-7 — including the collection-root type, Phase-A review
// F2: the authored ontology covers what the code stamps, not section 6's
// list verbatim).
constexpr char kTypePrefs[] = "legion://types/ChromePreferences";
constexpr char kTypePref[] = "legion://types/ChromePreference";

// The LIVE profile's PrefService, reached the BrowserList way
// (tabs_overview precedent) and through the component-layer
// user_prefs::UserPrefs association — no profile-monolith dependency.
PrefService* LiveProfilePrefs() {
  Browser* last = chrome::FindLastActive();
  if (!last) {
    return nullptr;
  }
  content::WebContents* wc = last->tab_strip_model()->GetActiveWebContents();
  if (!wc) {
    return nullptr;
  }
  return user_prefs::UserPrefs::Get(wc->GetBrowserContext());
}

const char* TypeName(base::Value::Type type) {
  switch (type) {
    case base::Value::Type::NONE:
      return "null";
    case base::Value::Type::BOOLEAN:
      return "boolean";
    case base::Value::Type::INTEGER:
      return "integer";
    case base::Value::Type::DOUBLE:
      return "double";
    case base::Value::Type::STRING:
      return "string";
    case base::Value::Type::BINARY:
      return "binary";
    case base::Value::Type::DICT:
      return "dictionary";
    case base::Value::Type::LIST:
      return "list";
  }
  return "unknown";
}

// legion://chrome/prefs/<name> — ONE registered preference, projected
// from the registry at ask time (stateless; the live PrefService is the
// only state, and it is the host's).
class PrefNode : public Handle {
 public:
  explicit PrefNode(std::string name)
      : name_(std::move(name)),
        identity_(std::string(kPrefsUri) + "/" + name_) {}

  StateKind state_kind() const override { return StateKind::ResolvedValue; }
  const Value& resolved_value() const override { return identity_; }
  std::shared_ptr<Handle> resolved_handle() const override { return nullptr; }
  std::string_view broken_reason() const override { return ""; }
  std::string sturdy_identity() const override {
    return identity_.as_string();
  }

  std::shared_ptr<Handle> ask_impl(std::string_view msg,
                                   const Value& spec) override {
    if (msg == "__getIdentity") {
      return ValueHandle::make(identity_);
    }
    if (msg == "__getType") {
      return ValueHandle::make(Value(std::string(kTypePref)));
    }
    PrefService* prefs = LiveProfilePrefs();
    if (!prefs) {
      return ValueHandle::make_broken("no-live-profile");
    }
    const PrefService::Preference* pref = prefs->FindPreference(name_);
    if (!pref) {
      return ValueHandle::make_broken("unknown-pref");
    }
    if (msg == "__getSchema") {
      // The REGISTRY's type + default (the host's registration — design
      // section 4: names, types, defaults from the registry).
      const base::Value* default_value = prefs->GetDefaultPrefValue(name_);
      return ValueHandle::make(Value::make_object({
          {"name", Value(name_)},
          {"type", Value(std::string(TypeName(pref->GetType())))},
          {"default",
           default_value ? FromBaseValue(*default_value) : Value()},
          {"isDefault", Value(pref->IsDefaultValue())},
          {"isUserModifiable", Value(pref->IsUserModifiable())},
      }));
    }
    if (msg == "get") {
      const base::Value* value = pref->GetValue();
      return ValueHandle::make(value ? FromBaseValue(*value) : Value());
    }
    if (msg == "set") {
      return Set(prefs, pref, spec);
    }
    return ValueHandle::make_broken("unknown-message");
  }

  void tell(std::string_view, const Value&) override {}

 private:
  // The write path, gated by the registry's type: the host's own
  // SetUserPrefValue NOTREACHED-crashes on a type mismatch, so the
  // mirror refuses typed BEFORE the host — never a crash, never a
  // silent flatten (value-only conversion).
  std::shared_ptr<Handle> Set(PrefService* prefs,
                              const PrefService::Preference* pref,
                              const Value& spec) {
    const Value* value = spec.object_get("value");
    if (!value) {
      return ValueHandle::make_broken("set-needs-value");
    }
    std::optional<base::Value> converted = ToBaseValue(*value);
    if (!converted) {
      return ValueHandle::make_broken("set-value-not-convertible");
    }
    if (converted->type() != pref->GetType()) {
      return ValueHandle::make_broken(
          std::string("pref-type-mismatch: ") + name_ + " is " +
          TypeName(pref->GetType()) + ", got " + TypeName(converted->type()));
    }
    prefs->Set(name_, *converted);
    // Answer the new EFFECTIVE value (a managed pref shows unchanged —
    // the host's truth, not the caller's intent).
    const base::Value* effective = pref->GetValue();
    return ValueHandle::make(effective ? FromBaseValue(*effective) : Value());
  }

  const std::string name_;
  const Value identity_;
};

// legion://chrome/prefs — the registry root. Every __getChildren is a
// fresh iteration of the live profile's registered preferences.
class PrefsMirrorNode : public Handle {
 public:
  PrefsMirrorNode() = default;

  StateKind state_kind() const override { return StateKind::ResolvedValue; }
  const Value& resolved_value() const override { return identity_; }
  std::shared_ptr<Handle> resolved_handle() const override { return nullptr; }
  std::string_view broken_reason() const override { return ""; }
  std::string sturdy_identity() const override { return kPrefsUri; }

  std::shared_ptr<Handle> ask_impl(std::string_view msg,
                                   const Value& /*spec*/) override {
    if (msg == "__getIdentity") {
      return ValueHandle::make(identity_);
    }
    if (msg == "__getType") {
      return ValueHandle::make(Value(std::string(kTypePrefs)));
    }
    if (msg == "__getChildren" || msg == "__getSchema") {
      PrefService* prefs = LiveProfilePrefs();
      if (!prefs) {
        return ValueHandle::make_broken("no-live-profile");
      }
      std::vector<Value> children;
      prefs->IteratePreferenceValues(base::BindRepeating(
          [](std::vector<Value>* out, const std::string& key,
             const base::Value& /*value*/) {
            out->emplace_back(std::string(kPrefsUri) + "/" + key);
          },
          &children));
      if (msg == "__getSchema") {
        return ValueHandle::make(Value::make_object({
            {"count", Value(static_cast<int64_t>(children.size()))},
        }));
      }
      return ValueHandle::make(Value::make_array(std::move(children)));
    }
    if (!msg.empty() && msg.front() == '_') {
      return ValueHandle::make_broken("unknown-message");
    }
    // A child segment is a preference name (names carry dots, never
    // slashes, so one path segment is one name).
    PrefService* prefs = LiveProfilePrefs();
    if (!prefs) {
      return ValueHandle::make_broken("no-live-profile");
    }
    if (!prefs->FindPreference(std::string(msg))) {
      return ValueHandle::make_broken("unknown-pref");
    }
    return std::make_shared<PrefNode>(std::string(msg));
  }

  void tell(std::string_view, const Value&) override {}

 private:
  Value identity_{std::string(kPrefsUri)};
};

}  // namespace

std::shared_ptr<velite::agentspaces::Handle> CreatePrefsMirror() {
  return std::make_shared<PrefsMirrorNode>();
}

}  // namespace aurelian
