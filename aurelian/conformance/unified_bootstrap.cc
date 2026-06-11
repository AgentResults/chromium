// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/conformance/unified_bootstrap.h"

#include <set>
#include <utility>

#include "aurelian/conformance/facet_claims.h"
#include "velite/agentspaces-wire/peer_session.hpp"

namespace aurelian {

std::shared_ptr<velite::agentspaces::ConformanceBootstrap>
BuildUnifiedBootstrap(
    const std::string& explicit_seed,
    std::shared_ptr<velite::agentspaces::Handle> chrome_root) {
  auto bootstrap =
      velite::agentspaces::wire::make_seeded_bootstrap(explicit_seed);

  // The chrome mount, pre-seal (design §4.1: the embodiment-install
  // primitive; [EMBODIMENT-RESOLVER-IS-MOUNT-TABLE] discharged by
  // construction — getResource resolves chrome through the same table as
  // every vendored facility).
  bootstrap->mount_resource("legion://chrome", std::move(chrome_root));
  bootstrap->seal();

  // ONE claims set, two projections (design §3.2): this cap and the emitted
  // manifest are projections of the same facet_claims source.
  std::set<std::string> claims;
  for (const std::string& f : aurelian_claimed_wire_facets()) {
    claims.insert(f);
  }
  for (const std::string& f : aurelian_claimed_internal_facets()) {
    claims.insert(f);
  }
  bootstrap->apply_claims_cap(
      std::make_shared<const std::set<std::string>>(std::move(claims)));
  return bootstrap;
}

}  // namespace aurelian
