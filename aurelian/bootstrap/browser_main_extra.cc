// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/bootstrap/browser_main_extra.h"

#include <algorithm>
#include <map>
#include <memory>
#include <vector>

#include <cstdlib>
#include <string>

#include "aurelian/capability/cap_anchor_provisioner.h"
#include "aurelian/federation/uds_register.h"
#include "aurelian/handles/browser/tab_handle.h"
#include "aurelian/handles/root/root_handle.h"
#include "aurelian/media/aurelian_virtual_camera.h"
#include "aurelian/media/aurelian_virtual_mic.h"
#include "aurelian/membrane/embodiment_policy.h"
#include "aurelian/membrane/install.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/synchronization/waitable_event.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_list.h"
#include "chrome/browser/ui/browser_list_observer.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model_observer.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/web_contents.h"
#include "velite/agentspaces-wire/agentspace.hpp"

namespace aurelian {
namespace {

int64_t NextTabId() {
  static int64_t next = 1;
  return next++;
}

// The generation-stable registration UDS Agrippa serves (the machine-fed
// Frontinus pattern). A connectivity string read but never branched on — the
// AGRIPPA_UDS_PATH override is the benign CLAUDE.md carveout (like PORT).
std::string ResolveAgrippaSock() {
  if (const char* p = std::getenv("AGRIPPA_UDS_PATH")) {
    return p;
  }
  const char* home = std::getenv("HOME");
  return std::string(home ? home : "/tmp") + "/.legion/agrippa.sock";
}

// The operator/machine cap trust anchor (32-byte Ed25519 pub) the spawning
// Agrippa publishes to this embodiment, hex-encoded. Pure data the app reads but
// does NOT branch on — the benign connectivity carveout (like a key path). Empty
// / malformed => no anchor provisioned (the membrane stays fail-closed).
std::vector<uint8_t> ResolveCapAnchor() {
  const char* hex = std::getenv("AURELIAN_CAP_ANCHOR");
  std::vector<uint8_t> bytes;
  if (hex && *hex) {
    base::HexStringToBytes(hex, &bytes);
  }
  return bytes;
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
  // C9 — the machine-federation register-in (dials Agrippa's UDS, registers the
  // `chrome` facet; no inbound port). Stopped on teardown (dtor → Stop()).
  std::unique_ptr<UdsRegister> uds_register;
  // C-MEDIA-2c — the avatar virtual camera, registered with the video capture
  // service at boot and pumping frames live from MediaSeam (which Cicero's
  // video_sink fills). Destroyed on teardown (drops the device pipes).
  std::unique_ptr<AurelianVirtualCamera> virtual_camera;
  // C-MEDIA-2e — the TTS virtual mic, draining MediaSeam audio into the
  // shared-memory ring the fork's FakeAudioInputStream reads. Destroyed on
  // teardown (stops the pump + unmaps the ring).
  std::unique_ptr<AurelianVirtualMic> virtual_mic;

  // tab_id -> impl (owned here; registry in tab_handle.cc mirrors)
  std::map<content::WebContents*, std::unique_ptr<TabHandleImpl>> tab_impls;
  // wc -> tab_id mapping
  std::map<content::WebContents*, int64_t> wc_ids;
  // AU-CAP-LIVE — the operator cap trust anchor + a per-WebContents provisioner
  // that pushes it onto each renderer frame's membrane (so the real cap crypto
  // actually runs in production instead of fail-closing to cap-untrusted-anchor).
  std::vector<uint8_t> cap_anchor;
  std::map<content::WebContents*, std::unique_ptr<CapAnchorProvisioner>>
      cap_provisioners;

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
    // AU-CAP-LIVE — provision the operator anchor onto this tab's renderer
    // frames as they are created. A 32-byte anchor was published at boot.
    if (cap_anchor.size() == 32) {
      cap_provisioners[wc] =
          std::make_unique<CapAnchorProvisioner>(wc, cap_anchor);
    }
  }

  void UnmountTab(content::WebContents* wc) {
    auto it = tab_impls.find(wc);
    if (it != tab_impls.end()) {
      DestroyTabHandle(std::move(it->second));
      tab_impls.erase(it);
    }
    cap_provisioners.erase(wc);
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

  // AU-CAP-LIVE — read the operator cap trust anchor Agrippa published, so the
  // per-tab CapAnchorProvisioner can push it onto renderer membranes at mount.
  impl_->cap_anchor = ResolveCapAnchor();

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

  // 5. C9 — register the SEALED chrome facet INTO Agrippa over the local UDS
  //    (AURELIAN-DESIGN §3.6/§13; the machine-fed Frontinus pattern). Aurelian
  //    opens NO inbound network port; the hub forwards a controller's leaf asks
  //    back as dispatchAt against the sealed root. A missing hub is fail-soft —
  //    the browser still works, just unregistered until the hub is up.
  //    Forwarded asks arrive on UdsRegister's serve thread, but browser handles
  //    are UI-thread-affine, so dispatch is refused off the UI thread (returns
  //    broken) rather than touching a browser object off-thread (a crash); the
  //    UI-thread hop for forwarded asks is the next slice (C9-forward-threading).
  //    The register handshake itself invokes no dispatch, so it is fully live.
  ChromeRoot* sealed_root = impl_->root;
  impl_->uds_register = std::make_unique<UdsRegister>();
  const std::string sock = ResolveAgrippaSock();
  const bool dialed = impl_->uds_register->Start(
      sock, "chrome", "aurelian-browser",
      [sealed_root](const std::string& path) -> std::string {
        // Forwarded asks arrive on UdsRegister's serve thread; browser handles
        // are UI-thread affine (touching a WebContents off-thread crashes —
        // physics, not permission). Hop to the UI thread, run RootDispatch
        // there, and return its reply. In production the UI message loop runs
        // normally so the posted task executes; a test must spin the loop.
        if (content::BrowserThread::CurrentlyOn(content::BrowserThread::UI)) {
          return RootDispatch(sealed_root, path);
        }
        std::string result;
        base::WaitableEvent done;
        content::GetUIThreadTaskRunner({})->PostTask(
            FROM_HERE,
            base::BindOnce(
                [](ChromeRoot* r, const std::string& p, std::string* out,
                   base::WaitableEvent* d) {
                  *out = RootDispatch(r, p);
                  d->Signal();
                },
                sealed_root, path, &result, &done));
        done.Wait();
        return result;
      });
  LOG(WARNING) << "[aurelian] machine-hub register "
               << (dialed ? "dialed + registered facet chrome over"
                          : "no hub at")
               << " " << sock;
}

void BrowserMainExtra::PreBrowserStart() {
  // Start observing before the first browser is created so we catch it.
  impl_->StartObserving();
}

void BrowserMainExtra::PostBrowserStart() {
  // C-MEDIA-2c — register the avatar virtual camera with the real video capture
  // service at boot + start its frame pump. From here a page's getUserMedia (or
  // a Meet join) opening the camera receives the avatar frames Cicero's
  // video_sink writes into MediaSeam. The browser loop is running now, so the
  // pump timer + mojo remotes have a live UI-thread sequence.
  impl_->virtual_camera = std::make_unique<AurelianVirtualCamera>();
  impl_->virtual_camera->Start();

  // C-MEDIA-2e — open the TTS mic ring + start its drain pump at boot, so the
  // fork's FakeAudioInputStream reads the agent's voice (Cicero's audio_sink ->
  // MediaSeam -> this ring) as the browser microphone.
  impl_->virtual_mic = std::make_unique<AurelianVirtualMic>();
  impl_->virtual_mic->Start();
}

void BrowserMainExtra::PostMainMessageLoopRun() {
  // Tear down before the browser shuts down.
  impl_.reset();
}

}  // namespace aurelian
