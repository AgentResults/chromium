// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/handles/root/root_handle.h"

#include <memory>
#include <string>
#include <vector>

#include "aurelian/handles/root/wire_serialize.h"

#include "aurelian/handles/browser/gpu_handle.h"
#include "aurelian/handles/browser/system_handle.h"
#include "aurelian/handles/browser/tabs_overview.h"
#include "aurelian/handles/browser/tabstrip_handle.h"
#include "aurelian/handles/media/media_handles.h"
#include "aurelian/handles/media/media_seam.h"
#include "aurelian/membrane/embodiment_policy.h"
#include "aurelian/mirror/cdp_mirror.h"
#include "base/strings/string_number_conversions.h"
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
    // No leaf handle exposes an unwrap path to its ambient reference; any
    // unknown message (including __ambient / unwrap probes) is refused.
    return ValueHandle::make_broken("unknown-message");
  }

  void tell(std::string_view, const Value&) override {}

 private:
  Value identity_{std::string("legion://chrome/system")};
};

// legion://chrome/gpu — the browser's collected GPU info, reached from the
// root (global query, no Browser* held).
class GpuInfoHandle : public Handle {
 public:
  StateKind state_kind() const override { return StateKind::ResolvedValue; }
  const Value& resolved_value() const override { return identity_; }
  std::shared_ptr<Handle> resolved_handle() const override { return nullptr; }
  std::string_view broken_reason() const override { return ""; }
  std::string sturdy_identity() const override { return "legion://chrome/gpu"; }

  std::shared_ptr<Handle> ask_impl(std::string_view msg,
                                   const Value& /*spec*/) override {
    if (msg == "__getIdentity") {
      return ValueHandle::make(Value(std::string("legion://chrome/gpu")));
    }
    if (msg == "info") {
      GpuSummary gpu = GetGpuSummary();
      return ValueHandle::make(Value::make_object({
          {"vendorId", Value(static_cast<int>(gpu.vendor_id))},
          {"deviceId", Value(static_cast<int>(gpu.device_id))},
          {"glVendor", Value(gpu.gl_vendor)},
          {"glRenderer", Value(gpu.gl_renderer)},
      }));
    }
    // No leaf handle exposes an unwrap path to its ambient reference; any
    // unknown message (including __ambient / unwrap probes) is refused.
    return ValueHandle::make_broken("unknown-message");
  }

  void tell(std::string_view, const Value&) override {}

 private:
  Value identity_{std::string("legion://chrome/gpu")};
};

// legion://chrome/tabs/<index> — a single live tab, reached by a controller
// walking the root. The interactive verbs (activate / close) mutate the live
// strip via the last-active browser; `url` reads its committed location. This
// is the per-tab reach that AU-TAB-REACH wires up: tabstrip_handle built these
// ops but, until mounted here, nothing could reach them.
class MountedTabHandle : public Handle {
 public:
  explicit MountedTabHandle(int index)
      : index_(index),
        identity_(std::string("legion://chrome/tabs/") +
                  std::to_string(index)) {}

  StateKind state_kind() const override { return StateKind::ResolvedValue; }
  const Value& resolved_value() const override { return identity_; }
  std::shared_ptr<Handle> resolved_handle() const override { return nullptr; }
  std::string_view broken_reason() const override { return ""; }

  std::shared_ptr<Handle> ask_impl(std::string_view msg,
                                   const Value& /*spec*/) override {
    if (msg == "__getIdentity") {
      return ValueHandle::make(identity_);
    }
    if (msg == "activate") {
      return ValueHandle::make(Value(std::string(
          ActivateTabGlobal(index_) ? "ok" : "bad-index")));
    }
    if (msg == "close") {
      // CloseTabGlobal refuses a bad index AND the browser's last tab.
      return ValueHandle::make(Value(std::string(
          CloseTabGlobal(index_) ? "ok" : "refused")));
    }
    if (msg == "url") {
      return ValueHandle::make(Value(TabUrlGlobal(index_)));
    }
    return ValueHandle::make_broken("unknown-message");
  }

  void tell(std::string_view, const Value&) override {}

 private:
  int index_;
  Value identity_;
};

// legion://chrome/tabs — the live, browser-wide tab strip reached from the
// root (no Browser* held; queries BrowserList). Read verbs (count, activeUrl,
// activeIndex) and the interactive `open`; a numeric child segment resolves to
// the MountedTabHandle for that tab (activate / close / url).
class TabsOverviewHandle : public Handle {
 public:
  StateKind state_kind() const override { return StateKind::ResolvedValue; }
  const Value& resolved_value() const override { return identity_; }
  std::shared_ptr<Handle> resolved_handle() const override { return nullptr; }
  std::string_view broken_reason() const override { return ""; }
  std::string sturdy_identity() const override {
    return "legion://chrome/tabs";
  }

  std::shared_ptr<Handle> ask_impl(std::string_view msg,
                                   const Value& /*spec*/) override {
    if (msg == "__getIdentity") {
      return ValueHandle::make(Value(std::string("legion://chrome/tabs")));
    }
    if (msg == "count") {
      return ValueHandle::make(Value(GetTabsOverview().open_count));
    }
    if (msg == "activeUrl") {
      return ValueHandle::make(Value(GetTabsOverview().active_url));
    }
    if (msg == "activeIndex") {
      return ValueHandle::make(Value(ActiveTabIndexGlobal()));
    }
    // Interactive: open a new (foreground) tab; returns its index, -1 on
    // failure. Param-free (about:blank) — the controller then navigates the tab
    // via the per-tab handle / nav surface.
    if (msg == "open") {
      return ValueHandle::make(Value(OpenTabGlobal("about:blank", true)));
    }
    // A numeric child segment reaches a single live tab: legion://chrome/tabs/2
    // -> MountedTabHandle(2), exposing activate / close / url.
    int index = 0;
    if (base::StringToInt(msg, &index)) {
      return std::make_shared<MountedTabHandle>(index);
    }
    // No leaf handle exposes an unwrap path to its ambient reference; any
    // unknown message (including __ambient / unwrap probes) is refused.
    return ValueHandle::make_broken("unknown-message");
  }

  void tell(std::string_view, const Value&) override {}

 private:
  Value identity_{std::string("legion://chrome/tabs")};
};

// legion://chrome/ — the navigable root and the install-membrane's only
// ingress. A capability child is mounted ONLY when the install policy allows
// it (the seal, AURELIAN-DESIGN.md §4.5); an unauthorised reach to ambient
// browser state fails.
class ChromeRootHandle : public Handle {
 public:
  explicit ChromeRootHandle(EmbodimentPolicy policy)
      : policy_(std::move(policy)) {
    // The media surface holds state (buffered frames, peer-audio
    // subscriptions) so it MUST be a persistent child, not minted fresh per
    // ask. Wired to the process-global MediaSeam the content-layer capture
    // device reads. Created only when the seal grants `media`.
    if (policy_.Allows("media")) {
      media_ = CreateMediaHandle(&MediaSeam::Get());
    }
    // ACM-1: the CDP catalog mirror. Persistent for the same reason as
    // media_ — the mirror is the ONE instance the design's residency rule
    // names (section 3: per-target session state hangs off the mirror from
    // ACM-2 on, so it must not be minted fresh per ask).
    if (policy_.Allows("cdp")) {
      cdp_ = CreateCdpMirror();
    }
  }

  StateKind state_kind() const override { return StateKind::ResolvedValue; }
  const Value& resolved_value() const override { return identity_; }
  std::shared_ptr<Handle> resolved_handle() const override { return nullptr; }
  std::string_view broken_reason() const override { return ""; }
  std::string sturdy_identity() const override { return "legion://chrome/"; }

  std::shared_ptr<Handle> ask_impl(std::string_view msg,
                                   const Value& /*spec*/) override {
    // Identity verbs are not ambient authority; always answerable.
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

    // Capability mounts — gated by the policy. A mountable name the policy
    // does not grant resolves broken("out-of-scope"); no ambient state leaks.
    const std::string name(msg);
    if (name == "media") {
      if (!policy_.Allows("media") || !media_) {
        return ValueHandle::make_broken("out-of-scope");
      }
      return media_;
    }
    if (name == "cdp") {
      if (!policy_.Allows("cdp") || !cdp_) {
        return ValueHandle::make_broken("out-of-scope");
      }
      return cdp_;
    }
    if (name == "system" || name == "tabs" || name == "gpu") {
      if (!policy_.Allows(name)) {
        return ValueHandle::make_broken("out-of-scope");
      }
      if (name == "system") {
        return std::make_shared<SystemInfoHandle>();
      }
      if (name == "tabs") {
        return std::make_shared<TabsOverviewHandle>();
      }
      return std::make_shared<GpuInfoHandle>();
    }

    // Introspection / unwrap probes (__ambient, unwrap, …) never yield a raw
    // ambient reference.
    if (!name.empty() && name.front() == '_') {
      return ValueHandle::make_broken("unknown-message");
    }

    // Any other name is a capability the membrane does not mount — an
    // unauthorised reach, out of scope.
    return ValueHandle::make_broken("out-of-scope");
  }

  void tell(std::string_view, const Value&) override {}

 private:
  Value identity_{std::string("legion://chrome/")};
  EmbodimentPolicy policy_;
  // Persistent media surface (legion://chrome/media), or null when unsealed.
  std::shared_ptr<Handle> media_;
  // Persistent CDP catalog mirror (legion://chrome/cdp), or null when
  // unsealed (ACM-1).
  std::shared_ptr<Handle> cdp_;
};

// Serializes a settled handle's reply to the federation-wire string form. The
// complete, escaping-correct implementation lives in wire_serialize.cc (shared
// + unit-tested); this is a thin alias kept for call-site readability.
std::string Serialize(const std::shared_ptr<Handle>& h) {
  return SerializeWireReply(h);
}

}  // namespace

struct ChromeRoot {
  std::shared_ptr<Handle> handle;
};

ChromeRoot* CreateChromeRoot() {
  return CreateChromeRootWithPolicy(EmbodimentPolicy::FullStandalone());
}

ChromeRoot* CreateChromeRootWithPolicy(const EmbodimentPolicy& policy) {
  auto* root = new ChromeRoot();
  root->handle = std::make_shared<ChromeRootHandle>(policy);
  return root;
}

void DestroyChromeRoot(ChromeRoot* root) {
  delete root;
}

DispatchOutcome RootDispatch(ChromeRoot* root, const std::string& path) {
  DispatchOutcome out;
  if (!root || !root->handle) {
    out.reply = "broken:no-root";
    return out;
  }
  // Walk the path segment by segment from the root. Each non-final segment must
  // resolve to a child handle; the final segment is asked and its reply
  // serialized. Empty segments (leading/trailing/double slash) are skipped.
  std::vector<std::string> segments = base::SplitString(
      path, "/", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  if (segments.empty()) {
    out.reply = Serialize(root->handle);
    return out;
  }
  std::shared_ptr<Handle> cur = root->handle;
  for (size_t i = 0; i + 1 < segments.size(); ++i) {
    cur = cur->ask(segments[i], Value());
    if (!cur || cur->state_kind() == StateKind::Broken) {
      out.reply = Serialize(cur);
      return out;
    }
  }
  std::shared_ptr<Handle> answer = cur->ask(segments.back(), Value());
  if (answer && answer->state_kind() == StateKind::Pending) {
    // HS-1 (design section 3): the answer settles in a LATER UI turn — the
    // HS-1 session layer owns settlement delivery. SerializeWireReply never
    // sees a Pending handle; the caller owns the wait (the wire bridge waits
    // on its completion record; an in-process caller pumps its own loop).
    out.kind = DispatchOutcome::Kind::kPending;
    out.answer = std::move(answer);
    return out;
  }
  out.reply = Serialize(answer);
  return out;
}

}  // namespace aurelian
