// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/bootstrap/browser_main_extra.h"

#include <algorithm>
#include <map>
#include <memory>
#include <vector>

#include "aurelian/handles/browser/tab_handle.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/task/single_thread_task_runner.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_list.h"
#include "chrome/browser/ui/browser_list_observer.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model_observer.h"
#include "content/public/browser/web_contents.h"
#include "velite/agentspaces-wire/actorspace.hpp"
#include "velite/agentspaces-wire/handle.hpp"
#include "velite/agentspaces-wire/value_handle.hpp"

namespace aurelian {
namespace {

// A simple root handle for legion://chrome/ (carried over from C0).
class ChromeRootHandle : public velite::agentspaces::Handle {
 public:
  static std::shared_ptr<ChromeRootHandle> make() {
    return std::shared_ptr<ChromeRootHandle>(new ChromeRootHandle());
  }

  velite::agentspaces::StateKind state_kind() const override {
    return velite::agentspaces::StateKind::ResolvedValue;
  }
  const velite::agentspaces::Value& resolved_value() const override {
    return value_;
  }
  std::shared_ptr<velite::agentspaces::Handle> resolved_handle()
      const override {
    return nullptr;
  }
  std::string_view broken_reason() const override { return ""; }
  std::string sturdy_identity() const override { return "legion://chrome/"; }

  std::shared_ptr<velite::agentspaces::Handle> ask_impl(
      std::string_view msg,
      const velite::agentspaces::Value& /*spec*/) override {
    using V = velite::agentspaces::Value;
    using VH = velite::agentspaces::ValueHandle;
    if (msg == "__getIdentity") return VH::make(V("legion://chrome/"));
    if (msg == "describe") {
      return VH::make(V::make_object({
          {"name", V("chrome")},
          {"uri", V("legion://chrome/")},
          {"embodiment", V("aurelian")},
      }));
    }
    return VH::make_broken("not-callable");
  }

  void tell(std::string_view, const velite::agentspaces::Value&) override {}

 private:
  ChromeRootHandle() : value_("legion://chrome/") {}
  velite::agentspaces::Value value_;
};

int64_t NextTabId() {
  static int64_t next = 1;
  return next++;
}

}  // namespace

// ---------------------------------------------------------------------------
// BrowserMainExtraImpl — owns the ActorSpace, handles, and observers
// ---------------------------------------------------------------------------
struct BrowserMainExtraImpl : public BrowserListObserver,
                              public TabStripModelObserver {
  std::shared_ptr<velite::agentspaces::ActorSpace> actor_space;
  std::shared_ptr<ChromeRootHandle> root;
  std::unique_ptr<TabsHandleImpl> tabs_handle;

  // tab_id -> impl (owned here; registry in tab_handle.cc mirrors)
  std::map<content::WebContents*, std::unique_ptr<TabHandleImpl>> tab_impls;
  // wc -> tab_id mapping
  std::map<content::WebContents*, int64_t> wc_ids;

  ~BrowserMainExtraImpl() override {
    BrowserList::RemoveObserver(this);
    for (Browser* b : observed_browsers_) {
      b->tab_strip_model()->RemoveObserver(this);
    }
  }

  void StartObserving() {
    BrowserList::AddObserver(this);
  }

  std::vector<Browser*> observed_browsers_;

  void ObserveBrowser(Browser* browser) {
    observed_browsers_.push_back(browser);
    auto* model = browser->tab_strip_model();
    model->AddObserver(this);
    // Mount existing tabs (browser may already have tabs).
    for (int i = 0; i < model->count(); ++i) {
      MountTab(model->GetWebContentsAt(i));
    }
  }

  void MountTab(content::WebContents* wc) {
    if (wc_ids.count(wc)) return;  // already mounted
    int64_t id = NextTabId();
    wc_ids[wc] = id;
    tab_impls[wc] = CreateTabHandle(wc, id);
  }

  void UnmountTab(content::WebContents* wc) {
    auto it = tab_impls.find(wc);
    if (it != tab_impls.end()) {
      DestroyTabHandle(std::move(it->second));
      tab_impls.erase(it);
    }
    wc_ids.erase(wc);
  }

  // BrowserListObserver:
  void OnBrowserAdded(Browser* browser) override {
    ObserveBrowser(browser);
  }

  void OnBrowserRemoved(Browser* browser) override {
    browser->tab_strip_model()->RemoveObserver(this);
    observed_browsers_.erase(
        std::remove(observed_browsers_.begin(), observed_browsers_.end(),
                    browser),
        observed_browsers_.end());
  }

  // TabStripModelObserver:
  void OnTabStripModelChanged(
      TabStripModel* model,
      const TabStripModelChange& change,
      const TabStripSelectionChange& selection) override {
    if (change.type() == TabStripModelChange::kInserted) {
      for (const auto& inserted : change.GetInsert()->contents) {
        MountTab(inserted.contents);
      }
    } else if (change.type() == TabStripModelChange::kRemoved) {
      for (const auto& removed : change.GetRemove()->contents) {
        UnmountTab(removed.contents);
      }
    }
  }
};

// ---------------------------------------------------------------------------
// BrowserMainExtra lifecycle
// ---------------------------------------------------------------------------
BrowserMainExtra::BrowserMainExtra() = default;
BrowserMainExtra::~BrowserMainExtra() = default;

void BrowserMainExtra::PostCreateThreads() {
  impl_ = std::make_unique<BrowserMainExtraImpl>();

  // 1. Create the browser-process ActorSpace.
  impl_->actor_space =
      velite::agentspaces::ActorSpace::make("chrome-browser");

  // 2. Mount the root handle.
  impl_->root = ChromeRootHandle::make();

  // 3. Create the tabs handle.
  impl_->tabs_handle = CreateTabsHandle();

  // 4. Prove C0 still works.
  auto identity_handle =
      impl_->root->ask("__getIdentity", velite::agentspaces::Value());
  std::string identity_result;
  if (identity_handle &&
      identity_handle->state_kind() ==
          velite::agentspaces::StateKind::ResolvedValue &&
      identity_handle->resolved_value().is_string()) {
    identity_result = identity_handle->resolved_value().as_string();
  } else {
    identity_result = "<failed>";
  }
  LOG(WARNING) << "[aurelian] legion://chrome/ mounted; __getIdentity="
               << identity_result;
}

void BrowserMainExtra::PreBrowserStart() {
  // Start observing before the first browser is created so we catch it.
  impl_->StartObserving();
}

void BrowserMainExtra::PostBrowserStart() {
  // Schedule C1 self-test after a short delay so the initial tab is loaded.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE, base::BindOnce([]() {
        int failures = RunC1SelfTest();
        if (failures > 0) {
          LOG(ERROR) << "[aurelian-c1] SELF-TEST FAILED: " << failures
                     << " assertion(s)";
        }
      }),
      base::Seconds(3));
}

void BrowserMainExtra::PostMainMessageLoopRun() {
  // Tear down before the browser shuts down.
  impl_.reset();
}

}  // namespace aurelian
