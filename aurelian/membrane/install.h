// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian install-membrane (AURELIAN-DESIGN.md §4.5): the ONE-SHOT,
// privileged transformation — at genesis — from full ambient browser authority
// to a bounded graph of Handles under a sealed legion://chrome/ root. This is
// the only realisation of handle.md §9's "initial conditions" rule for the
// Chromium domain. Called from the genesis seam
// (aurelian::BrowserMainExtra::PostCreateThreads).

#ifndef AURELIAN_MEMBRANE_INSTALL_H_
#define AURELIAN_MEMBRANE_INSTALL_H_

namespace velite::agentspaces {
class AgentSpace;
}  // namespace velite::agentspaces

namespace aurelian {

class EmbodimentPolicy;
struct ChromeRoot;

// One-shot install of the Chromium embodiment membrane onto `space`. Consumes
// the browser process's ambient authority exactly once and mounts the
// policy-allowed capabilities as Handles under a sealed root. Returns the
// membrane's only ingress (the legion://chrome/ root), or nullptr if `space`
// was already embodied (legion://errors/EmbodimentInstallComplete). The
// one-shot is PER-AGENTSPACE — constructing a fresh installer against an
// already-embodied space does not re-open the membrane.
ChromeRoot* InstallChromeEmbodiment(velite::agentspaces::AgentSpace& space,
                                    const EmbodimentPolicy& policy);

// Whether `space` has been embodied and not yet uninstalled.
bool IsEmbodimentInstalled(const velite::agentspaces::AgentSpace& space);

// Revoke the membrane: cascade-revokes `space` (force-breaking every mounted
// Handle per [EMBODIMENT-MEMBRANE-REVOCABLE]), releases the one-shot guard, and
// destroys the root. `root` may be null.
void UninstallChromeEmbodiment(velite::agentspaces::AgentSpace& space,
                               ChromeRoot* root);

}  // namespace aurelian

#endif  // AURELIAN_MEMBRANE_INSTALL_H_
