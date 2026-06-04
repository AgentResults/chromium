// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/handles/root/root_handle.h"

#include <memory>
#include <string>
#include <vector>

#include "aurelian/handles/browser/system_handle.h"
#include "base/strings/string_split.h"
#include "velite/agentspaces-wire/handle.hpp"
#include "velite/agentspaces-wire/value_handle.hpp"

namespace aurelian {

namespace {

using velite::agentspaces::Handle;
using velite::agentspaces::StateKind;
using velite::agentspaces::Value;
using velite::agentspaces::ValueHandle;

// legion://chrome/system — a real browser capability reached by navigating from
// the root.
class SystemInfoHandle : public Handle {
 public:
  StateKind state_kind() const override { return StateKind::ResolvedValue; }
  const Value& resolved_value() const override { return identity_; }
  std::shared_ptr<Handle> resolved_handle() const override { return nullptr; }
  std::string_view broken_reason() const override { return ""; }
  std::string sturdy_identity() const override {
    return "legion://chrome/system";
  }

  std::shared_ptr<Handle> ask_impl(std::string_view msg,
                                   const Value& /*spec*/) override {
    if (msg == "__getIdentity") {
      return ValueHandle::make(Value(std::string("legion://chrome/system")));
    }
    if (msg == "info") {
      SystemInfo info = GetSystemInfo();
      return ValueHandle::make(Value::make_object({
          {"browserPid", Value(info.browser_pid)},
          {"rendererCount", Value(info.render_process_count)},
      }));
    }
    return ValueHandle::make_broken("not-callable");
  }

  void tell(std::string_view, const Value&) override {}

 private:
  Value identity_{std::string("legion://chrome/system")};
};

// legion://chrome/ — the navigable root.
class ChromeRootHandle : public Handle {
 public:
  StateKind state_kind() const override { return StateKind::ResolvedValue; }
  const Value& resolved_value() const override { return identity_; }
  std::shared_ptr<Handle> resolved_handle() const override { return nullptr; }
  std::string_view broken_reason() const override { return ""; }
  std::string sturdy_identity() const override { return "legion://chrome/"; }

  std::shared_ptr<Handle> ask_impl(std::string_view msg,
                                   const Value& /*spec*/) override {
    if (msg == "__getIdentity") {
      return ValueHandle::make(Value(std::string("legion://chrome/")));
    }
    if (msg == "describe") {
      return ValueHandle::make(Value::make_object({
          {"name", Value(std::string("chrome"))},
          {"uri", Value(std::string("legion://chrome/"))},
          {"embodiment", Value(std::string("aurelian"))},
      }));
    }
    if (msg == "system") {
      return std::make_shared<SystemInfoHandle>();
    }
    return ValueHandle::make_broken("not-callable");
  }

  void tell(std::string_view, const Value&) override {}

 private:
  Value identity_{std::string("legion://chrome/")};
};

// Serializes a settled handle's reply to a compact string. Objects emit a
// minimal JSON object (string + int fields), matching the shapes the leaf
// capabilities return today.
std::string Serialize(const std::shared_ptr<Handle>& h) {
  if (!h) {
    return "broken:null";
  }
  if (h->state_kind() == StateKind::Broken) {
    return std::string("broken:") + std::string(h->broken_reason());
  }
  const Value& v = h->resolved_value();
  if (v.is_string()) {
    return v.as_string();
  }
  if (v.is_int()) {
    return std::to_string(v.as_int());
  }
  if (v.is_object()) {
    std::string out = "{";
    bool first = true;
    for (const auto& [key, val] : v.as_object()) {
      if (!first) {
        out += ",";
      }
      first = false;
      out += "\"" + key + "\":";
      if (val.is_string()) {
        out += "\"" + val.as_string() + "\"";
      } else if (val.is_int()) {
        out += std::to_string(val.as_int());
      } else {
        out += "null";
      }
    }
    out += "}";
    return out;
  }
  return "null";
}

}  // namespace

struct ChromeRoot {
  std::shared_ptr<Handle> handle;
};

ChromeRoot* CreateChromeRoot() {
  auto* root = new ChromeRoot();
  root->handle = std::make_shared<ChromeRootHandle>();
  return root;
}

void DestroyChromeRoot(ChromeRoot* root) {
  delete root;
}

std::string RootDispatch(ChromeRoot* root, const std::string& path) {
  if (!root || !root->handle) {
    return "broken:no-root";
  }
  // Walk the path segment by segment from the root. Each non-final segment must
  // resolve to a child handle; the final segment is asked and its reply
  // serialized. Empty segments (leading/trailing/double slash) are skipped.
  std::vector<std::string> segments = base::SplitString(
      path, "/", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  if (segments.empty()) {
    return Serialize(root->handle);
  }
  std::shared_ptr<Handle> cur = root->handle;
  for (size_t i = 0; i + 1 < segments.size(); ++i) {
    cur = cur->ask(segments[i], Value());
    if (!cur || cur->state_kind() == StateKind::Broken) {
      return Serialize(cur);
    }
  }
  return Serialize(cur->ask(segments.back(), Value()));
}

}  // namespace aurelian
