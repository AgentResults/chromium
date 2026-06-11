// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// ACM-6 (AURELIAN-GENERIC-CONTROL-DESIGN sections 2/6/8): the services
// catalog — legion://chrome/services — CATALOG-ONLY. C++ has no runtime
// method reflection, so Tier-2 invoke does not transfer: every service
// node answers `invoke` with the typed no-invoke-surface refusal (the
// honesty is load-bearing, asserted in the browsertest). The catalog is
// the KeyedServiceFactory dependency graph, enumerated through the
// one-line additive production accessor on DependencyManager (the
// recorded section-7 fork-delta line) via
// DependencyGraph::GetConstructionOrder. Mounted behind the sealed root
// when the EmbodimentPolicy grants `services`.

#ifndef AURELIAN_MIRROR_SERVICES_MIRROR_H_
#define AURELIAN_MIRROR_SERVICES_MIRROR_H_

#include <memory>

namespace velite::agentspaces {
class Handle;
}

namespace aurelian {

// The ONE services-catalog root node (legion://chrome/services).
std::shared_ptr<velite::agentspaces::Handle> CreateServicesMirror();

}  // namespace aurelian

#endif  // AURELIAN_MIRROR_SERVICES_MIRROR_H_
