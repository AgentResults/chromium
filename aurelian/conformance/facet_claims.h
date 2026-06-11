// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// CF-1 — the computed claims set (AURELIAN-CONFORMANT-FEDERATION-DESIGN.md
// §3.2): ONE source of truth, two projections. These sets feed BOTH the
// emitted __handshake manifest (emit_session_handshake_with_facets) AND the
// bootstrap's exhibit-side cap (ConformanceBootstrap::apply_claims_cap), so
// claim ⟺ exhibit holds by construction in both directions.
//
// The entries are the §3.3b CLAIMED rows exactly — floor included —
// build-conditioned in facet_claims.cc by the SAME guards that condition the
// implementing surface (the conformance_bootstrap.cpp:43-84 __has_include
// self-configuration; nothing federated-kg/* while LEGION_HAS_FEDERATED_KG is
// undefined). A build-config change therefore moves the manifest and the
// surface TOGETHER, never apart.
//
// The machine-readable column block (claimed ∪ queued ∪ not-claimed ∪
// out-of-scope + dispositions + the closed no-row prefix list) lives beside
// this file in aurelian-claims-draft.yaml — kept in lockstep; CF-V's
// classifier asserts (this claimed set) ⊆ (the draft's claimed rows).

#ifndef AURELIAN_CONFORMANCE_FACET_CLAIMS_H_
#define AURELIAN_CONFORMANCE_FACET_CLAIMS_H_

#include <string>
#include <vector>

namespace aurelian {

// The claimed WIRE-facet manifest: {floor ∪ §3.3b claimed wire rows},
// canonical legion://facets/ URIs. Order: floor first, then by area.
const std::vector<std::string>& aurelian_claimed_wire_facets();

// The claimed INTERNAL-axis facets (§3.3b internal group: the mirror facet +
// the engine attestations this column's own vectors prove).
const std::vector<std::string>& aurelian_claimed_internal_facets();

}  // namespace aurelian

#endif  // AURELIAN_CONFORMANCE_FACET_CLAIMS_H_
