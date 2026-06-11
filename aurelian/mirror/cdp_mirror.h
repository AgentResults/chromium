// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// ACM-1 (AURELIAN-GENERIC-CONTROL-DESIGN section 4): the CDP catalog
// mirror — legion://chrome/cdp — projected from the embedded descriptor
// (ACM-S2's CdpCatalog). Read-only and data-driven: ONE node class, no
// per-domain code; adding browser control surface is a Chromium roll,
// never a new handle class. Mounted as a persistent child of the sealed
// legion://chrome/ root when the EmbodimentPolicy grants `cdp` (the
// mirror-state residency rule, design section 3).

#ifndef AURELIAN_MIRROR_CDP_MIRROR_H_
#define AURELIAN_MIRROR_CDP_MIRROR_H_

#include <memory>
#include <string>

namespace velite::agentspaces {
class Handle;
}

namespace aurelian {

// The ONE mirror root node (legion://chrome/cdp). Children are the
// descriptor's domains; a domain's children its commands and events.
std::shared_ptr<velite::agentspaces::Handle> CreateCdpMirror();

// ACM-3: a per-target sub-mirror root (legion://chrome/targets/<id>/cdp)
// — the SAME node class, scoped to ONE target's persistent session.
// `target_type` decides the invoke gate (design section 2): page/frame
// sub-mirrors refuse the DERIVED closed browser-only set and the authored
// per-command overrides (typed, naming the browser path); non-page
// sub-mirrors (tab/worker/worklet) are annotation-not-applicable and
// dispatch-and-pass-through with NO derived refusals (round-8).
std::shared_ptr<velite::agentspaces::Handle> CreateCdpMirrorForTarget(
    const std::string& target_id,
    const std::string& target_type);

}  // namespace aurelian

#endif  // AURELIAN_MIRROR_CDP_MIRROR_H_
