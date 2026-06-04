// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/bootstrap/browser_main_extra.h"

#include <algorithm>
#include <map>
#include <memory>
#include <vector>

#include "aurelian/handles/browser/tab_handle.h"
#include "aurelian/handles/root/root_handle.h"
#include "aurelian/membrane/embodiment_policy.h"
#include "aurelian/membrane/install.h"
#include "base/logging.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_list.h"
#include "chrome/browser/ui/browser_list_observer.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model_observer.h"
#include "content/public/browser/web_contents.h"
#include "velite/agentspaces-wire/agentspace.hpp"

namespace aurelian {
namespace {

int64_t NextTabId() {
  static int64_t next = 1;
  return next++;
}

}  // namespace

// ---------------------------------------------------------------------------
// BrowserMainExtraImpl — owns the AgentSpace, handles, and observers
// ---------------------------------------------------------------------------
struct BrowserMainExtraImpl : public BrowserListObserver,
                              public TabStripModelObserver {
  std::shared_ptr<velite::agentspaces::AgentSpace> actor_space;
  // The navigable legion://chrome/ root — the install-membrane's only ingress
  // (the single shared model, also served to remote peers by the federation
  // layer). Erected by the one-shot install; owned via the C-API.
  ChromeRoot* root = nullptr;
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
    // Revoke the whole membrane: cascade-revoke the AgentSpace (force-breaking
    // every mounted Handle) and destroy the root.
    if (actor_space) {
      UninstallChromeEmbodiment(*actor_space, root);
    } else {
      DestroyChromeRoot(root);
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

  // 1. Create the browser-process AgentSpace (the membrane carrier).
  impl_->actor_space =
      velite::agentspaces::AgentSpace::make("chrome-browser");

  // 2. THE ONE-SHOT INSTALL — the only consumer of ambient browser authority
  //    (AURELIAN-DESIGN.md §4.5). Standalone bring-up uses an explicit
  //    full-trust EmbodimentPolicy (typed + sealed + revocable), NOT an
  //    un-typed ambient self-grant. install mounts the policy-allowed
  //    capabilities under a sealed legion://chrome/ root.
  impl_->root = InstallChromeEmbodiment(*impl_->actor_space,
                                        EmbodimentPolicy::FullStandalone());

  // 3. Create the tabs handle.
  impl_->tabs_handle = CreateTabsHandle();

  // 4. Prove the membrane is erected + its root reachable: walk it for identity.
  std::string identity_result = RootDispatch(impl_->root, "__getIdentity");
  LOG(WARNING) << "[aurelian] legion://chrome/ install-membrane sealed; "
               << "__getIdentity=" << identity_result;
}

void BrowserMainExtra::PreBrowserStart() {
  // Start observing before the first browser is created so we catch it.
  impl_->StartObserving();
}

void BrowserMainExtra::PostBrowserStart() {
  // Self-tests ripped in C3.5 — now in *_browsertest.cc files.
}

void BrowserMainExtra::PostMainMessageLoopRun() {
  // Tear down before the browser shuts down.
  impl_.reset();
}

}  // namespace aurelian
