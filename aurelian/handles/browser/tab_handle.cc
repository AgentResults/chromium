// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/handles/browser/tab_handle.h"

#include <map>
#include <string>

#include "aurelian/public/mojom/aurelian_wire.mojom.h"
#include "base/logging.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"
#include "velite/agentspaces-wire/actorspace.hpp"
#include "velite/agentspaces-wire/handle.hpp"
#include "velite/agentspaces-wire/value_handle.hpp"

namespace aurelian {
namespace {

using V = velite::agentspaces::Value;
using VH = velite::agentspaces::ValueHandle;
using Handle = velite::agentspaces::Handle;
using StateKind = velite::agentspaces::StateKind;

// ---------------------------------------------------------------------------
// Global tab registry — maps tab_id to (WebContents*, shared TabHandle).
// All access on the UI thread.
// ---------------------------------------------------------------------------
struct TabEntry {
  content::WebContents* wc;
  std::shared_ptr<Handle> handle;
  int64_t id;
};

std::map<int64_t, TabEntry>& Registry() {
  static auto* reg = new std::map<int64_t, TabEntry>();
  return *reg;
}

// ---------------------------------------------------------------------------
// NavigationHandle — drives content::NavigationController
// ---------------------------------------------------------------------------
class AurelianNavigationHandle : public Handle {
 public:
  static std::shared_ptr<AurelianNavigationHandle> make(
      content::WebContents* wc,
      int64_t tab_id) {
    return std::shared_ptr<AurelianNavigationHandle>(
        new AurelianNavigationHandle(wc, tab_id));
  }

  StateKind state_kind() const override { return StateKind::ResolvedValue; }
  const V& resolved_value() const override { return value_; }
  std::shared_ptr<Handle> resolved_handle() const override { return nullptr; }
  std::string_view broken_reason() const override { return ""; }
  std::string sturdy_identity() const override { return uri_; }

  std::shared_ptr<Handle> ask_impl(std::string_view msg,
                                   const V& /*spec*/) override {
    if (!wc_) return VH::make_broken("gone");
    auto& ctrl = wc_->GetController();

    if (msg == "__getIdentity") return VH::make(V(uri_));
    if (msg == "canGoBack") return VH::make(V(ctrl.CanGoBack()));
    if (msg == "canGoForward") return VH::make(V(ctrl.CanGoForward()));
    if (msg == "currentIndex")
      return VH::make(V(static_cast<int64_t>(ctrl.GetCurrentEntryIndex())));
    if (msg == "entries")
      return VH::make(V(static_cast<int64_t>(ctrl.GetEntryCount())));
    return VH::make_broken("not-callable");
  }

  void tell(std::string_view msg, const V& data) override {
    if (!wc_) return;
    auto& ctrl = wc_->GetController();

    if (msg == "loadUrl") {
      if (data.is_string()) {
        content::NavigationController::LoadURLParams params(
            GURL(data.as_string()));
        params.transition_type = ui::PAGE_TRANSITION_TYPED;
        ctrl.LoadURLWithParams(params);
      } else if (data.is_object()) {
        const V* url_val = data.object_get("url");
        if (url_val && url_val->is_string()) {
          content::NavigationController::LoadURLParams params(
              GURL(url_val->as_string()));
          params.transition_type = ui::PAGE_TRANSITION_TYPED;
          ctrl.LoadURLWithParams(params);
        }
      }
    } else if (msg == "reload") {
      ctrl.Reload(content::ReloadType::NORMAL, false);
    } else if (msg == "goBack") {
      if (ctrl.CanGoBack()) ctrl.GoBack();
    } else if (msg == "goForward") {
      if (ctrl.CanGoForward()) ctrl.GoForward();
    } else if (msg == "stop") {
      wc_->Stop();
    }
  }

 private:
  AurelianNavigationHandle(content::WebContents* wc, int64_t tab_id)
      : wc_(wc),
        uri_("legion://chrome/browser/tabs/" +
             base::NumberToString(tab_id) + "/navigation"),
        value_(uri_) {}

  content::WebContents* wc_;
  std::string uri_;
  V value_;
};

// ---------------------------------------------------------------------------
// FrameHandle — dispatches verbs to renderer via Mojo AurelianWire
// ---------------------------------------------------------------------------
class AurelianFrameHandle : public Handle {
 public:
  static std::shared_ptr<AurelianFrameHandle> make(
      content::RenderFrameHost* rfh,
      int64_t tab_id) {
    return std::shared_ptr<AurelianFrameHandle>(
        new AurelianFrameHandle(rfh, tab_id));
  }

  StateKind state_kind() const override { return StateKind::ResolvedValue; }
  const V& resolved_value() const override { return value_; }
  std::shared_ptr<Handle> resolved_handle() const override { return nullptr; }
  std::string_view broken_reason() const override { return ""; }
  std::string sturdy_identity() const override { return uri_; }

  std::shared_ptr<Handle> ask_impl(std::string_view msg,
                                   const V& spec) override {
    DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
    if (!rfh_) return VH::make_broken("gone");

    if (msg == "__getIdentity") return VH::make(V(uri_));

    // All other verbs dispatch to the renderer over Mojo.
    std::string reply_str = MojoDispatch(std::string(msg), spec);
    if (reply_str.empty() || reply_str == "<not-bound>" ||
        reply_str == "<disconnected>") {
      return VH::make_broken("renderer-unreachable");
    }
    // Return the raw reply as a string value. The caller interprets.
    return VH::make(V(reply_str));
  }

  void tell(std::string_view msg, const V& data) override {
    DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
    if (!rfh_) return;
    MojoDispatch(std::string(msg), data);
  }

 private:
  AurelianFrameHandle(content::RenderFrameHost* rfh, int64_t tab_id)
      : rfh_(rfh),
        uri_("legion://chrome/browser/tabs/" +
             base::NumberToString(tab_id) + "/frames/" +
             base::NumberToString(rfh->GetRoutingID())),
        value_(uri_) {}

  // Synchronous Mojo dispatch: sends verb\tparam, waits for reply.
  // Uses base::RunLoop (safe on UI thread for Mojo IPC).
  std::string MojoDispatch(const std::string& verb, const V& spec) {
    if (!rfh_) return "<disconnected>";

    mojo::AssociatedRemote<aurelian::mojom::AurelianWire> wire;
    rfh_->GetRemoteAssociatedInterfaces()->GetInterface(&wire);
    if (!wire.is_bound()) return "<not-bound>";

    // Build envelope: "verb" or "verb\tparam"
    std::string msg = verb;
    if (spec.is_string()) {
      msg += "\t" + spec.as_string();
    } else if (spec.is_object()) {
      // For object specs, pass the first string value as param.
      for (auto& [k, v] : spec.as_object()) {
        if (v.is_string()) {
          msg += "\t" + v.as_string();
          break;
        }
      }
    }

    std::vector<uint8_t> envelope(msg.begin(), msg.end());
    std::string reply_str;
    bool disconnected = false;

    base::RunLoop run_loop(base::RunLoop::Type::kNestableTasksAllowed);
    wire.set_disconnect_handler(base::BindOnce(
        [](base::RunLoop* loop, bool* disc) {
          *disc = true;
          loop->Quit();
        },
        &run_loop, &disconnected));
    wire->Dispatch(
        envelope,
        base::BindOnce(
            [](base::RunLoop* loop, std::string* out,
               const std::vector<uint8_t>& reply) {
              *out = std::string(reply.begin(), reply.end());
              loop->Quit();
            },
            &run_loop, &reply_str));
    run_loop.Run();

    if (disconnected) return "<disconnected>";
    return reply_str;
  }

  content::RenderFrameHost* rfh_;
  std::string uri_;
  V value_;
};

// ---------------------------------------------------------------------------
// TabHandle — bound to a WebContents
// ---------------------------------------------------------------------------
class AurelianTabHandle : public Handle {
 public:
  static std::shared_ptr<AurelianTabHandle> make(content::WebContents* wc,
                                                 int64_t tab_id) {
    return std::shared_ptr<AurelianTabHandle>(
        new AurelianTabHandle(wc, tab_id));
  }

  StateKind state_kind() const override { return StateKind::ResolvedValue; }
  const V& resolved_value() const override { return value_; }
  std::shared_ptr<Handle> resolved_handle() const override { return nullptr; }
  std::string_view broken_reason() const override { return ""; }
  std::string sturdy_identity() const override { return uri_; }

  std::shared_ptr<Handle> ask_impl(std::string_view msg,
                                   const V& spec) override {
    if (!wc_) return VH::make_broken("gone");

    if (msg == "__getIdentity") return VH::make(V(uri_));
    if (msg == "url")
      return VH::make(V(wc_->GetVisibleURL().spec()));
    if (msg == "title")
      return VH::make(V(base::UTF16ToUTF8(wc_->GetTitle())));
    if (msg == "isLoading")
      return VH::make(V(wc_->IsLoading()));
    if (msg == "navigation")
      return AurelianNavigationHandle::make(wc_, tab_id_);
    if (msg == "describe") {
      return VH::make(V::make_object({
          {"uri", V(uri_)},
          {"url", V(wc_->GetVisibleURL().spec())},
          {"title", V(base::UTF16ToUTF8(wc_->GetTitle()))},
      }));
    }
    // frames: list frame routing IDs for this tab.
    if (msg == "frames") {
      std::vector<V> frame_ids;
      wc_->GetPrimaryMainFrame()->ForEachRenderFrameHost(
          [&](content::RenderFrameHost* rfh) {
            frame_ids.push_back(
                V(static_cast<int64_t>(rfh->GetRoutingID())));
          });
      return VH::make(V::make_array(std::move(frame_ids)));
    }
    // frame: return an AurelianFrameHandle for a specific frame.
    if (msg == "frame") {
      content::RenderFrameHost* target = nullptr;
      if (spec.is_object()) {
        const V* id_val = spec.object_get("id");
        if (id_val && id_val->is_int()) {
          int want = static_cast<int>(id_val->as_int());
          wc_->GetPrimaryMainFrame()->ForEachRenderFrameHost(
              [&](content::RenderFrameHost* rfh) {
                if (rfh->GetRoutingID() == want) target = rfh;
              });
        }
      }
      // Default: primary main frame.
      if (!target) target = wc_->GetPrimaryMainFrame();
      if (!target) return VH::make_broken("no-frame");
      return AurelianFrameHandle::make(target, tab_id_);
    }
    return VH::make_broken("not-callable");
  }

  void tell(std::string_view /*msg*/, const V& /*data*/) override {}

  content::WebContents* web_contents() const { return wc_; }
  int64_t tab_id() const { return tab_id_; }

 private:
  AurelianTabHandle(content::WebContents* wc, int64_t tab_id)
      : wc_(wc),
        tab_id_(tab_id),
        uri_("legion://chrome/browser/tabs/" +
             base::NumberToString(tab_id)),
        value_(uri_) {}

  content::WebContents* wc_;
  int64_t tab_id_;
  std::string uri_;
  V value_;
};

// ---------------------------------------------------------------------------
// TabsHandle — legion://chrome/browser/tabs
// ---------------------------------------------------------------------------
class AurelianTabsHandle : public Handle {
 public:
  static std::shared_ptr<AurelianTabsHandle> make() {
    return std::shared_ptr<AurelianTabsHandle>(new AurelianTabsHandle());
  }

  StateKind state_kind() const override { return StateKind::ResolvedValue; }
  const V& resolved_value() const override { return value_; }
  std::shared_ptr<Handle> resolved_handle() const override { return nullptr; }
  std::string_view broken_reason() const override { return ""; }
  std::string sturdy_identity() const override {
    return "legion://chrome/browser/tabs";
  }

  std::shared_ptr<Handle> ask_impl(std::string_view msg,
                                   const V& spec) override {
    if (msg == "__getIdentity")
      return VH::make(V("legion://chrome/browser/tabs"));

    if (msg == "list") {
      std::vector<V> items;
      for (auto& [id, entry] : Registry()) {
        if (!entry.wc) continue;
        items.push_back(V::make_object({
            {"id", V(id)},
            {"url", V(entry.wc->GetVisibleURL().spec())},
            {"title", V(base::UTF16ToUTF8(entry.wc->GetTitle()))},
            {"active", V(false)},  // simplified for C1
        }));
      }
      return VH::make(V::make_array(std::move(items)));
    }

    if (msg == "tab") {
      if (spec.is_object()) {
        const V* id_val = spec.object_get("id");
        if (id_val && id_val->is_int()) {
          auto it = Registry().find(id_val->as_int());
          if (it != Registry().end()) return it->second.handle;
          return VH::make_broken("not-found");
        }
      }
      return VH::make_broken("bad-spec");
    }
    return VH::make_broken("not-callable");
  }

  void tell(std::string_view /*msg*/, const V& /*data*/) override {}

 private:
  AurelianTabsHandle() : value_("legion://chrome/browser/tabs") {}
  V value_;
};

}  // namespace

// ---------------------------------------------------------------------------
// Helpers to stash/retrieve a shared_ptr<Handle> in a void*.
// ---------------------------------------------------------------------------
void StashHandle(void*& slot, std::shared_ptr<Handle> h) {
  slot = new std::shared_ptr<Handle>(std::move(h));
}

std::shared_ptr<Handle>& GetHandle(void* slot) {
  return *static_cast<std::shared_ptr<Handle>*>(slot);
}

void ClearHandle(void*& slot) {
  if (slot) {
    delete static_cast<std::shared_ptr<Handle>*>(slot);
    slot = nullptr;
  }
}

// ---------------------------------------------------------------------------
// Public API — called from BrowserMainExtra
// ---------------------------------------------------------------------------
TabHandleImpl::~TabHandleImpl() { ClearHandle(handle_ptr); }
TabsHandleImpl::~TabsHandleImpl() { ClearHandle(handle_ptr); }

std::unique_ptr<TabHandleImpl> CreateTabHandle(content::WebContents* wc,
                                               int64_t tab_id) {
  auto impl = std::make_unique<TabHandleImpl>();
  auto handle = AurelianTabHandle::make(wc, tab_id);
  StashHandle(impl->handle_ptr, handle);
  impl->tab_id = tab_id;

  TabEntry entry;
  entry.wc = wc;
  entry.handle = handle;
  entry.id = tab_id;
  Registry()[tab_id] = entry;

  LOG(WARNING) << "[aurelian] tab mounted: legion://chrome/browser/tabs/"
               << tab_id << " url=" << wc->GetVisibleURL().spec();
  return impl;
}

void DestroyTabHandle(std::unique_ptr<TabHandleImpl> impl) {
  if (impl) {
    Registry().erase(impl->tab_id);
    LOG(WARNING) << "[aurelian] tab unmounted: legion://chrome/browser/tabs/"
                 << impl->tab_id;
  }
}

std::unique_ptr<TabsHandleImpl> CreateTabsHandle() {
  auto impl = std::make_unique<TabsHandleImpl>();
  StashHandle(impl->handle_ptr, AurelianTabsHandle::make());
  return impl;
}

}  // namespace aurelian
