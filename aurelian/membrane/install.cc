// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/membrane/install.h"

#include <set>
#include <string>

#include "aurelian/handles/root/root_handle.h"
#include "aurelian/membrane/embodiment_policy.h"
#include "base/no_destructor.h"
#include "base/synchronization/lock.h"
#include "velite/agentspaces-wire/agentspace.hpp"

namespace aurelian {

namespace {

// Per-AgentSpace one-shot guard, keyed by the space's logical id (stable; no
// pointer-reuse hazard across space lifetimes). install may run on the UI
// thread while a federation thread holds a root, so the guard is locked.
base::Lock& GuardLock() {
  static base::NoDestructor<base::Lock> lock;
  return *lock;
}

std::set<std::string>& InstalledIds() {
  static base::NoDestructor<std::set<std::string>> ids;
  return *ids;
}

}  // namespace

ChromeRoot* InstallChromeEmbodiment(velite::agentspaces::AgentSpace& space,
                                    const EmbodimentPolicy& policy) {
  {
    base::AutoLock guard(GuardLock());
    if (InstalledIds().count(space.id()) != 0) {
      // legion://errors/EmbodimentInstallComplete — ambient authority is
      // consumed exactly once per AgentSpace.
      space.record_audit("embodiment-install-complete");
      return nullptr;
    }
    InstalledIds().insert(space.id());
  }

  // The one privileged transformation: mount the policy-allowed capabilities
  // under a sealed root. The captured ambient references live only inside the
  // mounted handles' closures.
  ChromeRoot* root = CreateChromeRootWithPolicy(policy);
  space.record_audit("embodiment-installed");
  return root;
}

bool IsEmbodimentInstalled(const velite::agentspaces::AgentSpace& space) {
  base::AutoLock guard(GuardLock());
  return InstalledIds().count(space.id()) != 0;
}

void UninstallChromeEmbodiment(velite::agentspaces::AgentSpace& space,
                               ChromeRoot* root) {
  // Cascade-revoke the membrane: every mounted Handle is force-broken.
  space.revoke();
  {
    base::AutoLock guard(GuardLock());
    InstalledIds().erase(space.id());
  }
  if (root) {
    DestroyChromeRoot(root);
  }
}

}  // namespace aurelian
