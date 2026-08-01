// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/handles/root/root_handle.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "aurelian/handles/root/wire_serialize.h"

#include "aurelian/handles/media/media_handles.h"
#include "aurelian/handles/media/media_seam.h"
#include "aurelian/membrane/embodiment_policy.h"
#include "aurelian/mirror/cdp_mirror.h"
#include "aurelian/mirror/cdp_session.h"
#include "aurelian/mirror/prefs_mirror.h"
#include "aurelian/mirror/services_mirror.h"
#include "aurelian/mirror/targets_mirror.h"
#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/web_contents.h"
#include "url/gurl.h"
#include "velite/agentspaces-wire/handle.hpp"
#include "velite/agentspaces-wire/json_marshal.hpp"
#include "velite/agentspaces-wire/value_handle.hpp"

namespace aurelian {

namespace {

using velite::agentspaces::Handle;
using velite::agentspaces::StateKind;
using velite::agentspaces::Value;
using velite::agentspaces::ValueHandle;

// ---------------------------------------------------------------------------
// ACM-R(5) — the FOLD facades (design section 7): the kept facade verbs are
// semantic conveniences whose IMPLEMENTATIONS delegate to the mirror — every
// host fact that the descriptor models rides the ONE CdpSessionRegistry
// (SystemInfo.* / Target.* / Page.*), reshaped at settle time into the
// facade's convenience shape by the HS-1 layer itself (no parallel dispatch
// machinery). What remains inline below is the recorded strip/selection
// GLUE of the inventory FOLD table: active index, index->tab resolution,
// the last-tab guard, and the committed-URL read — host-UI concepts the
// descriptor does not model.
// ---------------------------------------------------------------------------

// Reads `key` from an object Value (null when absent / not an object).
const Value* FindField(const Value& v, const std::string& key) {
  if (!v.is_object()) {
    return nullptr;
  }
  auto it = v.as_object().find(key);
  return it == v.as_object().end() ? nullptr : &it->second;
}

// CDP numbers parse int-or-double depending on magnitude; the facade
// reshapes read them leniently.
int64_t AsIntLenient(const Value& v) {
  if (v.is_int()) {
    return v.as_int();
  }
  if (v.is_double()) {
    return static_cast<int64_t>(v.as_double());
  }
  return 0;
}

// -- The recorded strip/selection glue (inventory FOLD table) --

content::WebContents* TabAtIndexGlue(int index) {
  Browser* browser = chrome::FindLastActive();
  if (!browser) {
    return nullptr;
  }
  TabStripModel* model = browser->tab_strip_model();
  if (index < 0 || index >= model->count()) {
    return nullptr;
  }
  return model->GetWebContentsAt(index);
}

int StripIndexOfGlue(content::WebContents* wc) {
  Browser* browser = chrome::FindLastActive();
  if (!browser || !wc) {
    return -1;
  }
  const int index = browser->tab_strip_model()->GetIndexOfWebContents(wc);
  return index == TabStripModel::kNoTab ? -1 : index;
}

int ActiveTabIndexGlue() {
  Browser* browser = chrome::FindLastActive();
  return browser ? browser->tab_strip_model()->active_index() : -1;
}

std::string ActiveTabUrlGlue() {
  Browser* browser = chrome::FindLastActive();
  if (!browser) {
    return std::string();
  }
  content::WebContents* wc = browser->tab_strip_model()->GetActiveWebContents();
  return wc ? wc->GetLastCommittedURL().spec() : std::string();
}

// legion://chrome/system — the system facade. FOLD: `info` is
// SystemInfo.getProcessInfo through the browser-mirror session, reshaped at
// settle to the facade's {browserPid, rendererCount} convenience shape.
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
      return CdpSessionRegistry::Get().InvokeOnBrowserTarget(
          "SystemInfo.getProcessInfo", Value(),
          base::BindOnce([](Value result) {
            int64_t browser_pid = 0;
            int64_t renderer_count = 0;
            if (const Value* infos = FindField(result, "processInfo");
                infos && infos->is_array()) {
              for (const Value& entry : infos->as_array()) {
                const Value* type = FindField(entry, "type");
                const Value* id = FindField(entry, "id");
                if (!type || !type->is_string() || !id) {
                  continue;
                }
                if (type->as_string() == "browser") {
                  browser_pid = AsIntLenient(*id);
                } else if (type->as_string() == "renderer") {
                  ++renderer_count;
                }
              }
            }
            return Value::make_object({
                {"browserPid", Value(browser_pid)},
                {"rendererCount", Value(renderer_count)},
            });
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

// legion://chrome/gpu — the gpu facade. FOLD: `info` is SystemInfo.getInfo
// through the browser-mirror session, reshaped at settle to the facade's
// {vendorId, deviceId, glVendor, glRenderer} shape (devices[0] + the
// auxAttributes glVendor/glRenderer names gpu_info.cc writes).
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
      return CdpSessionRegistry::Get().InvokeOnBrowserTarget(
          "SystemInfo.getInfo", Value(),
          base::BindOnce([](Value result) {
            int64_t vendor_id = 0;
            int64_t device_id = 0;
            std::string gl_vendor;
            std::string gl_renderer;
            if (const Value* gpu = FindField(result, "gpu")) {
              if (const Value* devices = FindField(*gpu, "devices");
                  devices && devices->is_array() &&
                  !devices->as_array().empty()) {
                const Value& device = devices->as_array().front();
                if (const Value* v = FindField(device, "vendorId")) {
                  vendor_id = AsIntLenient(*v);
                }
                if (const Value* v = FindField(device, "deviceId")) {
                  device_id = AsIntLenient(*v);
                }
              }
              if (const Value* aux = FindField(*gpu, "auxAttributes")) {
                if (const Value* v = FindField(*aux, "glVendor");
                    v && v->is_string()) {
                  gl_vendor = v->as_string();
                }
                if (const Value* v = FindField(*aux, "glRenderer");
                    v && v->is_string()) {
                  gl_renderer = v->as_string();
                }
              }
            }
            return Value::make_object({
                {"vendorId", Value(vendor_id)},
                {"deviceId", Value(device_id)},
                {"glVendor", Value(gl_vendor)},
                {"glRenderer", Value(gl_renderer)},
            });
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
// walking the root. FOLD: `activate` is Page.bringToFront on THAT target's
// session; `close` is Target.closeTarget (after the pre-dispatch last-tab
// guard — a window close is out of scope); `url` is the committed-URL glue
// read. Index->target resolution is the recorded selection glue.
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
      content::WebContents* wc = TabAtIndexGlue(index_);
      if (!wc) {
        return ValueHandle::make(Value(std::string("bad-index")));
      }
      return CdpSessionRegistry::Get().InvokeOnTarget(
          content::DevToolsAgentHost::GetOrCreateFor(wc)->GetId(),
          "Page.bringToFront", Value(),
          base::BindOnce([](Value) { return Value(std::string("ok")); }));
    }
    if (msg == "close") {
      content::WebContents* wc = TabAtIndexGlue(index_);
      Browser* browser = chrome::FindLastActive();
      if (!wc || !browser || browser->tab_strip_model()->count() <= 1) {
        // A bad index or the browser's last tab (a window close is out of
        // scope) — refused pre-dispatch, the recorded guard glue.
        return ValueHandle::make(Value(std::string("refused")));
      }
      return CdpSessionRegistry::Get().InvokeOnBrowserTarget(
          "Target.closeTarget",
          Value::make_object(
              {{"targetId",
                Value(content::DevToolsAgentHost::GetOrCreateFor(wc)
                          ->GetId())}}),
          base::BindOnce([](Value result) {
            const Value* success = FindField(result, "success");
            return Value(std::string(
                success && success->is_bool() && !success->as_bool()
                    ? "refused"
                    : "ok"));
          }));
    }
    if (msg == "url") {
      content::WebContents* wc = TabAtIndexGlue(index_);
      return ValueHandle::make(
          Value(wc ? wc->GetLastCommittedURL().spec() : std::string()));
    }
    return ValueHandle::make_broken("unknown-message");
  }

  void tell(std::string_view, const Value&) override {}

 private:
  int index_;
  Value identity_;
};

// legion://chrome/tabs — the live, browser-wide tab strip reached from the
// root. FOLD: `count` is Target.getTargets reshaped to the open-page count;
// `open` is Target.createTarget with the settle-time strip-index lookup;
// activeUrl/activeIndex are the recorded strip/selection glue. A numeric
// child segment resolves to the MountedTabHandle for that tab.
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
      return CdpSessionRegistry::Get().InvokeOnBrowserTarget(
          "Target.getTargets", Value(),
          base::BindOnce([](Value result) {
            int64_t count = 0;
            if (const Value* infos = FindField(result, "targetInfos");
                infos && infos->is_array()) {
              for (const Value& info : infos->as_array()) {
                const Value* type = FindField(info, "type");
                if (type && type->is_string() &&
                    type->as_string() == "page") {
                  ++count;
                }
              }
            }
            return Value(count);
          }));
    }
    if (msg == "activeUrl") {
      return ValueHandle::make(Value(ActiveTabUrlGlue()));
    }
    if (msg == "activeIndex") {
      return ValueHandle::make(Value(ActiveTabIndexGlue()));
    }
    // Interactive: open a new (foreground) tab; settles to its strip index,
    // -1 on failure. Param-free (about:blank) — the controller then
    // navigates the tab via its targets/<id>/cdp/... sub-mirror.
    if (msg == "open") {
      return CdpSessionRegistry::Get().InvokeOnBrowserTarget(
          "Target.createTarget",
          Value::make_object({{"url", Value(std::string("about:blank"))}}),
          base::BindOnce([](Value result) {
            // The settle-time strip-index lookup (recorded glue): resolve
            // the created targetId to its WebContents and report the strip
            // index the facade contract promises.
            const Value* target_id = FindField(result, "targetId");
            if (!target_id || !target_id->is_string()) {
              return Value(-1);
            }
            scoped_refptr<content::DevToolsAgentHost> host =
                content::DevToolsAgentHost::GetForId(target_id->as_string());
            return Value(
                StripIndexOfGlue(host ? host->GetWebContents() : nullptr));
          }));
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

// ACM-2w one-root audit observable: every ChromeRootHandle construction in
// this process, counted (design section 5 — the WSS scaffold's lazy second
// root was a second consumption of ambient authority; the counter is what
// the RED pins at 1).
std::atomic<int> g_chrome_root_constructions{0};

// legion://chrome/ — the navigable root and the install-membrane's only
// ingress. A capability child is mounted ONLY when the install policy allows
// it (the seal, AURELIAN-DESIGN.md §4.5); an unauthorised reach to ambient
// browser state fails.
class ChromeRootHandle : public Handle {
 public:
  explicit ChromeRootHandle(EmbodimentPolicy policy)
      : policy_(std::move(policy)) {
    g_chrome_root_constructions.fetch_add(1, std::memory_order_relaxed);
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
    // ACM-3: the targets mirror (the per-target session state lives in the
    // process-global session registry; the root node is persistent per the
    // same residency rule).
    if (policy_.Allows("targets")) {
      targets_ = CreateTargetsMirror();
    }
    // ACM-5: the prefs mirror (stateless projection over the live
    // profile's PrefService; persistent per the same residency pattern).
    if (policy_.Allows("prefs")) {
      prefs_ = CreatePrefsMirror();
    }
    // ACM-6: the services catalog (catalog-only — design section 8).
    if (policy_.Allows("services")) {
      services_ = CreateServicesMirror();
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
    if (name == "targets") {
      if (!policy_.Allows("targets") || !targets_) {
        return ValueHandle::make_broken("out-of-scope");
      }
      return targets_;
    }
    if (name == "prefs") {
      if (!policy_.Allows("prefs") || !prefs_) {
        return ValueHandle::make_broken("out-of-scope");
      }
      return prefs_;
    }
    if (name == "services") {
      if (!policy_.Allows("services") || !services_) {
        return ValueHandle::make_broken("out-of-scope");
      }
      return services_;
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
  // Persistent targets mirror (legion://chrome/targets), or null when
  // unsealed (ACM-3).
  std::shared_ptr<Handle> targets_;
  // Persistent prefs mirror (legion://chrome/prefs), or null when
  // unsealed (ACM-5).
  std::shared_ptr<Handle> prefs_;
  // Persistent services catalog (legion://chrome/services), or null when
  // unsealed (ACM-6).
  std::shared_ptr<Handle> services_;
};

// Serializes a settled handle's reply to the federation-wire string form. The
// complete, escaping-correct implementation lives in wire_serialize.cc (shared
// + unit-tested); this is a thin alias kept for call-site readability.
WireReply Serialize(const std::shared_ptr<Handle>& h) {
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

int ChromeRootConstructionCountForTesting() {
  return g_chrome_root_constructions.load(std::memory_order_relaxed);
}

DispatchOutcome RootDispatch(ChromeRoot* root,
                             const std::string& path,
                             const std::string& serialized_spec) {
  DispatchOutcome out;
  if (!root || !root->handle) {
    out.reply = WireReply::MakeBroken("no-root");
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
  // HS-3 (ACM-2w): the serialized spec is parsed at this seam and handed to
  // the FINAL hop only — intermediate hops stay nullary navigation. A
  // malformed spec answers typed, never a silent empty-spec dispatch.
  Value final_spec;
  if (!serialized_spec.empty()) {
    velite::json::JsonValue parsed;
    if (!velite::json::JsonValue::parse(serialized_spec, parsed)) {
      out.reply = WireReply::MakeBroken("spec-parse-failure");
      return out;
    }
    final_spec = velite::agentspaces::from_json(parsed);
  }
  std::shared_ptr<Handle> cur = root->handle;
  for (size_t i = 0; i + 1 < segments.size(); ++i) {
    cur = cur->ask(segments[i], Value());
    if (!cur || cur->state_kind() == StateKind::Broken) {
      out.reply = Serialize(cur);
      return out;
    }
  }
  std::shared_ptr<Handle> answer = cur->ask(segments.back(), final_spec);
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
