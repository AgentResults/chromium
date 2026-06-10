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

namespace velite::agentspaces {
class Handle;
}

namespace aurelian {

// The ONE mirror root node (legion://chrome/cdp). Children are the
// descriptor's domains; a domain's children its commands and events.
std::shared_ptr<velite::agentspaces::Handle> CreateCdpMirror();

}  // namespace aurelian

#endif  // AURELIAN_MIRROR_CDP_MIRROR_H_
