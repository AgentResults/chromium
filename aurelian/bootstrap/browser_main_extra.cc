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
#include "aurelian/conformance/conformance_serve.h"
#include "aurelian/federation/bridge_dispatch.h"
#include "aurelian/federation/completion_bridge.h"
#include "aurelian/federation/uds_register.h"
#include "aurelian/handles/root/root_handle.h"
#include "aurelian/media/aurelian_virtual_camera.h"
#include "aurelian/media/aurelian_virtual_mic.h"
#include "aurelian/membrane/embodiment_policy.h"
#include "aurelian/membrane/install.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "chrome/browser/devtools/chrome_devtools_manager_delegate.h"
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

// CF-4 — the browser's own peer identity seed, PERSISTED under the operator
// config dir so the register-in destHash is stable across restarts
// ([EMBODIMENT-REGISTERED-CHILD-OWN-IDENTITY]; warm-restart preservation per
// bootstrap.md §1). Path override is the benign connectivity carveout
// (AURELIAN_PEER_SEED_PATH, like AGRIPPA_UDS_PATH). First boot mints 32
// random bytes (hex) and writes them 0600; every later boot reads the same
// seed back — one identity per operator install, never per process.
std::string ResolveAurelianPeerSeed() {
  std::string path;
  if (const char* p = std::getenv("AURELIAN_PEER_SEED_PATH"); p && *p) {
    path = p;
  } else {
    const char* home = std::getenv("HOME");
    path = std::string(home ? home : "/tmp") + "/.legion/aurelian/peer-seed";
  }
  // Boot-path identity I/O — raw syscalls (one tiny read, or a first-boot
  // write), the same shape as the UdsChannel::connect this thread performs
  // a few lines later; base's instrumented file APIs are friend-gated here.
  {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd >= 0) {
      char buf[129];
      const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
      ::close(fd);
      if (n > 0) {
        std::string seed(buf, static_cast<size_t>(n));
        while (!seed.empty() && (seed.back() == '\n' || seed.back() == ' ')) {
          seed.pop_back();
        }
        if (!seed.empty()) {
          return seed;
        }
      }
    }
  }
  uint8_t raw[32];
  base::RandBytes(raw);
  const std::string seed = base::ToLowerASCII(base::HexEncode(raw));
  // Best-effort persist (0600; parent dirs created). A write failure keeps
  // this boot's identity ephemeral — registration still works, stability
  // resumes once the operator config dir is writable.
  const size_t slash = path.rfind('/');
  if (slash != std::string::npos && slash > 0) {
    for (size_t pos = path.find('/', 1);
         pos != std::string::npos && pos <= slash;
         pos = path.find('/', pos + 1)) {
      ::mkdir(path.substr(0, pos).c_str(), 0700);  // EEXIST is fine
    }
    ::mkdir(path.substr(0, slash).c_str(), 0700);
  }
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd >= 0) {
    (void)!::write(fd, seed.data(), seed.size());
    ::close(fd);
  }
  return seed;
}

// The installed sealed root the ONE exported dispatch closes over — set by
// the one-shot install, cleared at teardown so a post-teardown dispatch is
// fail-closed ("broken:no-root"), never dangling.
ChromeRoot* g_installed_root = nullptr;

}  // namespace

// HS-3/ACM-2w (design section 5): the ONE ChromeDispatchFn over the
// INSTALLED root, constructed once — both wire bring-ups consume it; the
// WSS scaffold's lazy second root is deleted.
ChromeDispatchFn InstalledChromeDispatch() {
  static const base::NoDestructor<ChromeDispatchFn> dispatch(
      [](const std::string& path,
         const std::string& serialized_spec) -> std::string {
        ChromeRoot* root = g_installed_root;
        if (!root) {
          return "broken:no-root";
        }
        return BridgeDispatch(root, path, serialized_spec);
      });
  return *dispatch;
}

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
  // C9 — the machine-federation register-in (dials Agrippa's UDS, registers the
  // `chrome` facet; no inbound port). Stopped on teardown (dtor → Stop()).
  std::unique_ptr<UdsRegister> uds_register;
  // CF-1 — the conformance serve (dials the launcher's UDS under
  // --aurelian-conformance-serve; the canonical-runner bring-up). One wire
  // bring-up per process: this INSTEAD of the register-in when the switch
  // is present (the serve emits the __handshake manifest instead of the
  // Agrippa register ask, design §2.1). Stopped on teardown (dtor → Stop()).
  std::unique_ptr<ConformanceServe> conformance_serve;
  // C-MEDIA-2c — the avatar virtual camera, registered with the video capture
  // service at boot and pumping frames live from MediaSeam (which Cicero's
  // video_sink fills). Destroyed on teardown (drops the device pipes).
  std::unique_ptr<AurelianVirtualCamera> virtual_camera;
  // C-MEDIA-2e — the TTS virtual mic, draining MediaSeam audio into the
  // shared-memory ring the fork's FakeAudioInputStream reads. Destroyed on
  // teardown (stops the pump + unmaps the ring).
  std::unique_ptr<AurelianVirtualMic> virtual_mic;

  // wc -> tab_id mapping (the mount dedupe — a WebContents is observed once
  // however many strip events report it; ACM-R(4) kept the lifecycle hook +
  // this dedupe when the bespoke per-tab handles went, design section 7 F8)
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
    wc_ids[wc] = NextTabId();
    // AU-CAP-LIVE — provision the operator anchor onto this tab's renderer
    // frames as they are created. A 32-byte anchor was published at boot.
    if (cap_anchor.size() == 32) {
      cap_provisioners[wc] =
          std::make_unique<CapAnchorProvisioner>(wc, cap_anchor);
    }
  }

  void UnmountTab(content::WebContents* wc) {
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
  // HS-3: the installed root is what the ONE exported dispatch fn
  // (InstalledChromeDispatch) resolves against from here to teardown.
  g_installed_root = impl_->root;

  // 3. Prove the membrane is erected + its root reachable: walk it for
  //    identity (a settled answer — Completed by construction).
  std::string identity_result =
      RootDispatch(impl_->root, "__getIdentity").reply;
  LOG(WARNING) << "[aurelian] legion://chrome/ install-membrane sealed; "
               << "__getIdentity=" << identity_result;

  // 4. C9 — register the SEALED chrome facet INTO Agrippa over the local UDS
  //    (AURELIAN-DESIGN §3.6/§13; the machine-fed Frontinus pattern). Aurelian
  //    opens NO inbound network port; the hub forwards a controller's leaf asks
  //    back as dispatchAt against the sealed root. A missing hub is fail-soft —
  //    the browser still works, just unregistered until the hub is up.
  //    Forwarded asks arrive on UdsRegister's serve thread; browser handles
  //    are UI-thread affine (touching a WebContents off-thread crashes —
  //    physics, not permission). The register-in consumes the ONE exported
  //    dispatch fn (HS-3) whose BridgeDispatch is the HS-1
  //    completion-signaled hop (ACM-2, design section 3): the record is
  //    created on the serve thread pre-post, the posted UI task starts the
  //    dispatch and returns, and the serve thread waits on the record —
  //    Stop()-coverable, typed timeout, no UI-thread block.
  //    CF-1: under --aurelian-conformance-serve=<uds> the canonical-runner
  //    bring-up replaces the register-in for this process — the serve
  //    connects out to the launcher's UDS, mounts legion://chrome on the
  //    vendored bootstrap, and emits the COMPUTED __handshake manifest
  //    instead of the Agrippa register ask (design §2.1). Still no inbound
  //    listener in either mode.
  const base::CommandLine* command_line =
      base::CommandLine::ForCurrentProcess();
  if (command_line->HasSwitch(kConformanceServeSwitch)) {
    impl_->conformance_serve = std::make_unique<ConformanceServe>();
    const std::string serve_sock =
        command_line->GetSwitchValueASCII(kConformanceServeSwitch);
    const bool served = impl_->conformance_serve->Start(
        serve_sock, InstalledChromeDispatch(), impl_->cap_anchor,
        // Launcher lane: the connection IS the browser lifetime (design
        // §2.3-07). CloseBrowserSoon is the canonical programmatic-close
        // entry (the CDP Browser.close path): it releases the DevTools
        // keep-alive THEN ExitIgnoreUnloadHandlers — found at CF-1 that
        // a bare AttemptExit/ExitIgnoreUnloadHandlers leaves a headless
        // browser held open by that keep-alive. Posts to the UI thread
        // itself, so it is callable from the serve thread.
        base::BindOnce(&ChromeDevToolsManagerDelegate::CloseBrowserSoon));
    LOG(WARNING) << "[aurelian] conformance serve "
                 << (served ? "connected + handshake emitted over"
                            : "FAILED to connect")
                 << " " << serve_sock;
    return;
  }
  impl_->uds_register = std::make_unique<UdsRegister>();
  const std::string sock = ResolveAgrippaSock();
  // ACM-8: the federation seam shares the SAME operator anchor the renderer
  // membranes get — one trust root for every membrane in this embodiment.
  // CF-4: the register-in presents the browser's REAL persisted identity
  // (the seed → seeded Ed25519 destHash), replacing the old literal.
  const bool dialed = impl_->uds_register->Start(
      sock, "chrome", ResolveAurelianPeerSeed(), InstalledChromeDispatch(),
      impl_->cap_anchor);
  LOG(WARNING) << "[aurelian] machine-hub register "
               << (dialed ? "dialed + registered facet chrome over"
                          : "no hub at")
               << " " << sock
               << (dialed ? " destHash=" + impl_->uds_register->dest_hash()
                          : "");
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
  // HS-1 Stop() drain (ACM-2, design section 3): the UI loop has stopped
  // pumping, so a serve thread blocked on a completion record would never
  // be signalled by a completion — mark every live record shutdown and
  // signal BEFORE the teardown below joins the serve threads.
  CompletionBridge::Get().Stop();
  // The exported dispatch goes fail-closed before its root is destroyed.
  g_installed_root = nullptr;
  // Tear down before the browser shuts down.
  impl_.reset();
}

}  // namespace aurelian
