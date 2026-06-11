// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// ACM-3 (AURELIAN-GENERIC-CONTROL-DESIGN section 4): the targets mirror —
// legion://chrome/targets — the live target set over
// DevToolsAgentHost::GetOrCreateAll() (NOT GetAll(), review F3: a tab that
// never had a DevTools session must still appear; force-creation on
// enumeration is accepted — hosts are lightweight until attached, and
// sessions attach lazily per HS-1). Each targets/<id> node carries the CDP
// target type, typed (page, tab, service_worker, … — the mirror reflects
// what Chromium reports, design section 4), and its own /cdp/... sub-mirror
// where the page-scoped invokes land. Mounted behind the sealed root when
// the EmbodimentPolicy grants `targets` (per-slice policy growth).

#ifndef AURELIAN_MIRROR_TARGETS_MIRROR_H_
#define AURELIAN_MIRROR_TARGETS_MIRROR_H_

#include <memory>

namespace velite::agentspaces {
class Handle;
}

namespace aurelian {

// The ONE targets-mirror root node (legion://chrome/targets). Children are
// the live targets; a target's child `cdp` is its sub-mirror.
std::shared_ptr<velite::agentspaces::Handle> CreateTargetsMirror();

}  // namespace aurelian

#endif  // AURELIAN_MIRROR_TARGETS_MIRROR_H_
