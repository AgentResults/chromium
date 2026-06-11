// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// CF-4 — ONE unified slot-0 bootstrap construction, two bring-up callers
// (conformant-federation design §4.1): the seeded vendored
// ConformanceBootstrap with the chrome root mounted pre-seal
// (mount_resource — the Marius embodiment-install primitive), sealed, and
// claims-capped with the computed set (facet_claims). The conformance serve
// (CF-1) and the Agrippa register-in (CF-4) both build their slot-0 surface
// HERE — never two constructions that could drift.
//
// Velite-typed INTERNAL header (the nav_handle_internal.h discipline); the
// chrome root handle is passed in by the caller so this target depends only
// on the claims set + the vendored tree (no federation-layer cycle).

#ifndef AURELIAN_CONFORMANCE_UNIFIED_BOOTSTRAP_H_
#define AURELIAN_CONFORMANCE_UNIFIED_BOOTSTRAP_H_

#include <memory>
#include <string>

#include "velite/agentspaces-wire/conformance_bootstrap.hpp"

namespace aurelian {

// Build the unified bootstrap: seeded identity (explicit_seed; empty = the
// vendored VELITE_PEER_SEED env path), `chrome_root` mounted at
// legion://chrome BEFORE seal(), then the §3.2 claims cap applied (the same
// computed set the emitted manifest projects).
std::shared_ptr<velite::agentspaces::ConformanceBootstrap>
BuildUnifiedBootstrap(const std::string& explicit_seed,
                      std::shared_ptr<velite::agentspaces::Handle> chrome_root);

}  // namespace aurelian

#endif  // AURELIAN_CONFORMANCE_UNIFIED_BOOTSTRAP_H_
