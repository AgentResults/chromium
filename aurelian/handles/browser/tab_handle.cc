// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/handles/browser/tab_handle.h"

#include <map>
#include <sstream>
#include <string>

#include "aurelian/public/mojom/aurelian_wire.mojom.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/task/single_thread_task_runner.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
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
                                   const V& /*spec*/) override {
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

// ---------------------------------------------------------------------------
// C1 self-test — runs on UI thread, logs each assertion
// ---------------------------------------------------------------------------
int RunC1SelfTest() {
  int failures = 0;
  auto pass = [](const char* name) {
    LOG(WARNING) << "[aurelian-c1] PASS: " << name;
  };
  auto fail = [&failures](const char* name, const char* detail) {
    LOG(WARNING) << "[aurelian-c1] FAIL: " << name << " — " << detail;
    ++failures;
  };

  // 1. TabsHandle list
  auto tabs_handle = AurelianTabsHandle::make();
  auto list_result = tabs_handle->ask("list", V());
  if (list_result && list_result->state_kind() == StateKind::ResolvedValue &&
      list_result->resolved_value().is_array()) {
    size_t count = list_result->resolved_value().as_array().size();
    if (count > 0) {
      pass("tabs.list returns non-empty array");
    } else {
      fail("tabs.list returns non-empty array", "empty");
    }

    // Check first tab has url field
    if (count > 0) {
      const V& first = list_result->resolved_value().as_array()[0];
      const V* url_v = first.object_get("url");
      if (url_v && url_v->is_string() && !url_v->as_string().empty()) {
        pass("tabs.list[0] has url");
      } else {
        fail("tabs.list[0] has url", "missing or empty");
      }
    }
  } else {
    fail("tabs.list returns array", "not an array");
  }

  // 2. TabHandle reads url/title
  if (!Registry().empty()) {
    auto& [id, entry] = *Registry().begin();
    auto url_h = entry.handle->ask("url", V());
    if (url_h && url_h->state_kind() == StateKind::ResolvedValue &&
        url_h->resolved_value().is_string()) {
      pass("tab.url returns string");
      LOG(WARNING) << "[aurelian-c1]   url=" << url_h->resolved_value().as_string();
    } else {
      fail("tab.url returns string", "not a string");
    }

    auto title_h = entry.handle->ask("title", V());
    if (title_h && title_h->state_kind() == StateKind::ResolvedValue &&
        title_h->resolved_value().is_string()) {
      pass("tab.title returns string");
    } else {
      fail("tab.title returns string", "not a string");
    }

    auto loading_h = entry.handle->ask("isLoading", V());
    if (loading_h && loading_h->state_kind() == StateKind::ResolvedValue &&
        loading_h->resolved_value().is_bool()) {
      pass("tab.isLoading returns bool");
    } else {
      fail("tab.isLoading returns bool", "not a bool");
    }

    // 3. NavigationHandle
    auto nav_h = entry.handle->ask("navigation", V());
    if (nav_h && nav_h->state_kind() == StateKind::ResolvedValue) {
      pass("tab.navigation returns handle");

      auto entries_h = nav_h->ask("entries", V());
      if (entries_h && entries_h->state_kind() == StateKind::ResolvedValue &&
          entries_h->resolved_value().is_int()) {
        pass("nav.entries returns int");
      } else {
        fail("nav.entries returns int", "not an int");
      }

      auto can_back_h = nav_h->ask("canGoBack", V());
      if (can_back_h && can_back_h->state_kind() == StateKind::ResolvedValue &&
          can_back_h->resolved_value().is_bool()) {
        pass("nav.canGoBack returns bool");
      } else {
        fail("nav.canGoBack returns bool", "not a bool");
      }
    } else {
      fail("tab.navigation returns handle", "broken or null");
    }

    // 4. __getIdentity
    auto ident_h = entry.handle->ask("__getIdentity", V());
    if (ident_h && ident_h->state_kind() == StateKind::ResolvedValue &&
        ident_h->resolved_value().is_string() &&
        ident_h->resolved_value().as_string().find("legion://chrome/browser/tabs/") == 0) {
      pass("tab.__getIdentity correct");
    } else {
      fail("tab.__getIdentity correct", "wrong value");
    }
  } else {
    fail("registry has tabs", "empty");
  }

  LOG(WARNING) << "[aurelian-c1] self-test complete: "
               << (failures == 0 ? "ALL PASS" : "FAILURES: ")
               << (failures > 0 ? base::NumberToString(failures) : "");
  return failures;
}

// ---------------------------------------------------------------------------
// C2 self-test — dispatches "describe" to the renderer via Mojo
// ---------------------------------------------------------------------------
void RunC2SelfTest() {
  if (Registry().empty()) {
    LOG(WARNING) << "[aurelian-c2] FAIL: no tabs in registry for C2 test";
    return;
  }

  auto& [id, entry] = *Registry().begin();
  if (!entry.wc) {
    LOG(WARNING) << "[aurelian-c2] FAIL: WebContents is null";
    return;
  }

  content::RenderFrameHost* rfh = entry.wc->GetPrimaryMainFrame();
  if (!rfh) {
    LOG(WARNING) << "[aurelian-c2] FAIL: no primary main frame";
    return;
  }

  LOG(WARNING) << "[aurelian-c2] dispatching 'describe' to renderer frame "
               << rfh->GetRoutingID() << "...";

  // Use a leaked remote so the pipe stays alive for the async reply.
  auto* wire = new mojo::AssociatedRemote<aurelian::mojom::AurelianWire>();
  rfh->GetRemoteAssociatedInterfaces()->GetInterface(wire);

  if (!wire->is_bound()) {
    LOG(WARNING) << "[aurelian-c2] FAIL: AurelianWire not bound";
    delete wire;
    return;
  }

  LOG(WARNING) << "[aurelian-c2] PASS: AurelianWire bound to renderer";

  // Send "describe" as the envelope.
  std::string verb = "describe";
  std::vector<uint8_t> envelope(verb.begin(), verb.end());

  (*wire)->Dispatch(
      envelope,
      base::BindOnce([](mojo::AssociatedRemote<aurelian::mojom::AurelianWire>*
                            owned_wire,
                        const std::vector<uint8_t>& reply) {
                       std::string reply_str(reply.begin(), reply.end());
                       if (reply_str.find("renderer") != std::string::npos) {
                         LOG(WARNING)
                             << "[aurelian-c2] PASS: renderer replied: "
                             << reply_str;
                       } else {
                         LOG(WARNING)
                             << "[aurelian-c2] FAIL: unexpected reply: "
                             << reply_str;
                       }
                       LOG(WARNING) << "[aurelian-c2] self-test complete";
                       delete owned_wire;
                     },
                     wire));
}

// ---------------------------------------------------------------------------
// C3 self-test — DOM + JS over Mojo
// ---------------------------------------------------------------------------
void RunC3SelfTest() {
  if (Registry().empty()) {
    LOG(WARNING) << "[aurelian-c3] FAIL: no tabs in registry";
    return;
  }

  auto& [id, entry] = *Registry().begin();
  if (!entry.wc) {
    LOG(WARNING) << "[aurelian-c3] FAIL: WebContents is null";
    return;
  }

  // Navigate to a known page.
  std::string data_url =
      "data:text/html,<html><head><title>C3Test</title></head>"
      "<body><div id='target'>hello aurelian</div></body></html>";
  GURL test_url(data_url);
  content::NavigationController::LoadURLParams params(test_url);
  params.transition_type = ui::PAGE_TRANSITION_TYPED;
  entry.wc->GetController().LoadURLWithParams(params);

  // Wait for load, then run the "c3.selftest" verb on the renderer.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce([](content::WebContents* wc) {
        content::RenderFrameHost* rfh = wc->GetPrimaryMainFrame();
        if (!rfh) {
          LOG(WARNING) << "[aurelian-c3] FAIL: no primary main frame";
          return;
        }

        auto* wire =
            new mojo::AssociatedRemote<aurelian::mojom::AurelianWire>();
        rfh->GetRemoteAssociatedInterfaces()->GetInterface(wire);
        if (!wire->is_bound()) {
          LOG(WARNING) << "[aurelian-c3] FAIL: AurelianWire not bound";
          delete wire;
          return;
        }

        // Send "c3.selftest" — the renderer runs all DOM/JS checks
        // and returns results as lines of "PASS:name" or "FAIL:name".
        std::string verb = "c3.selftest";
        std::vector<uint8_t> envelope(verb.begin(), verb.end());
        (*wire)->Dispatch(
            envelope,
            base::BindOnce(
                [](mojo::AssociatedRemote<aurelian::mojom::AurelianWire>*
                       owned_wire,
                   const std::vector<uint8_t>& reply) {
                  std::string results(reply.begin(), reply.end());
                  // Log each line with [aurelian-c3] prefix.
                  std::istringstream iss(results);
                  std::string line;
                  while (std::getline(iss, line)) {
                    if (!line.empty()) {
                      LOG(WARNING) << "[aurelian-c3] " << line;
                    }
                  }
                  LOG(WARNING) << "[aurelian-c3] self-test complete";
                  delete owned_wire;
                },
                wire));
      }, entry.wc),
      base::Seconds(2));
}

}  // namespace aurelian
