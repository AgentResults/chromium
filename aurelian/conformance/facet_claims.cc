// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/conformance/facet_claims.h"

#include "base/no_destructor.h"

// Build-conditioning (design §3.2): an entry exists iff its implementing
// surface is compiled. The guards below are the SAME __has_include checks
// that light the vendored bootstrap's facility factories
// (conformance_bootstrap.cpp:43-84) — this TU compiles with the identical
// velite include path, so the conditions evaluate identically.
#if __has_include("velite/kg/entities.h")
#define AURELIAN_CLAIMS_HAS_KG 1
#endif
#if __has_include("velite/ontology/subsumption.h")
#define AURELIAN_CLAIMS_HAS_ONTOLOGY 1
#endif
#if __has_include("velite/resources/manifest.h")
#define AURELIAN_CLAIMS_HAS_RESOURCES 1
#endif

// Design §3.5(1)/§9.10 — the named BLOCKER, enforced at compile time: the
// federated-kg surface must NEVER enter the binary without claims-table +
// evidence work (the §3.3b build-excluded row covers all six corpus-routed
// federated-kg/ URIs + fedkg/ by name). If you hit this error, you defined
// the flag without re-deriving the table: do the §3.3 work first.
#ifdef LEGION_HAS_FEDERATED_KG
#error \
    "LEGION_HAS_FEDERATED_KG defined without claims-table work (design §3.5 BLOCKER)"
#endif

namespace aurelian {

namespace {
constexpr char kPrefix[] = "legion://facets/";

void Add(std::vector<std::string>* v, const char* suffix) {
  v->push_back(std::string(kPrefix) + suffix);
}
}  // namespace

const std::vector<std::string>& aurelian_claimed_wire_facets() {
  static const base::NoDestructor<std::vector<std::string>> claimed([] {
    std::vector<std::string> v;
    // --- The v0.1 universal floor (LAYERS.md §6; validate_wire_manifest_
    // floor requires all five).
    Add(&v, "envelopes/wire-canonical-signing-form");
    Add(&v, "protocol/wire-three-frames-only");
    Add(&v, "identity/wire-ed25519");
    Add(&v, "sessions/wire-handshake");
    Add(&v, "conduits/wire-at-least-one-declared");
    // --- Envelopes (the corpus routes the signing-form vectors here).
    Add(&v, "envelopes/canonical-signing-form");
    // --- L0 core.
    Add(&v, "conformance/wire-bootstrap-surface");
    Add(&v, "handles/wire-respond-to-ask");
    Add(&v, "handles/wire-accept-tells");
    Add(&v, "handles/wire-settlement");
    Add(&v, "revocation/wire-cascade-drop-frames");
    Add(&v, "revocation");
    Add(&v, "handoff/wire-three-vat-handshake");
    Add(&v, "handoff");
    Add(&v, "protocol/wire-slot-lifecycle");
    Add(&v, "errors/wire-typed-tree");
    // --- Dispatcher-core protocol family (12; §3.3b — uncappable core:
    // closure branch 1 treats a red as a DEFECT, never a downgrade).
    Add(&v, "protocol/wire-slot-zero-bootstrap");
    Add(&v, "protocol/wire-close-and-disconnect-settle");
    Add(&v, "protocol/wire-close-cascade");
    Add(&v, "protocol/wire-drop-propagation");
    Add(&v, "protocol/wire-drop-reason-disambiguates");
    Add(&v, "protocol/wire-pipelining-buffer");
    Add(&v, "protocol/wire-round-trip-identity");
    Add(&v, "protocol/wire-nested-handle-depth");
    Add(&v, "protocol/wire-placeholder-determined-fields");
    Add(&v, "protocol/wire-placeholder-redispatch-cross-as");
    Add(&v, "protocol/wire-substrate-retry-sequencing");
    Add(&v, "protocol/wire-uri-derivation-pure");
    // --- Errors family (4; uncappable core).
    Add(&v, "errors/wire-audit-records-refusal");
    Add(&v, "errors/wire-broken-state-carries-reason");
    Add(&v, "errors/wire-no-exceptions-on-wire");
    Add(&v, "errors/wire-propagation-pipeline");
    // --- Handle-core family (19, handles/v0.14; uncappable core).
    Add(&v, "handles/wire-two-verbs");
    Add(&v, "handles/wire-verbs-one-way");
    Add(&v, "handles/wire-no-throw");
    Add(&v, "handles/wire-reject-unknown-messages");
    Add(&v, "handles/wire-reference-equality-identity");
    Add(&v, "handles/wire-identity-immutable");
    Add(&v, "handles/wire-hardened-at-construction");
    Add(&v, "handles/wire-schema-hardened");
    Add(&v, "handles/wire-deterministic-dispatch");
    Add(&v, "handles/wire-four-acquisition-rules");
    Add(&v, "handles/wire-no-ambient-acquisition");
    Add(&v, "handles/wire-no-ambient-state-read");
    Add(&v, "handles/wire-no-extra-reserved-names");
    Add(&v, "handles/wire-no-value-equality-fallback");
    Add(&v, "handles/wire-internal-names-no-cross-boundary");
    Add(&v, "handles/wire-multi-shot-reserved");
    Add(&v, "handles/wire-methodmetadata-direct-ask-tell");
    Add(&v, "handles/wire-contract-bearing");
    Add(&v, "handles/wire-authorised-settler-only");
    // --- Handles introspection + area.
    Add(&v, "handles");
    Add(&v, "handles/contract-introspection");
    Add(&v, "handles/post-tell-ask-refactor");
    Add(&v, "handles/wire-__getType");
    Add(&v, "handles/wire-__getSchema");
    // --- Agentspace.
    Add(&v, "agentspace/wire-bootstrap-handle-at-slot-0");
    Add(&v, "agentspace/wire-revocation-cascades");
#if defined(AURELIAN_CLAIMS_HAS_KG)
    // --- kg (rides the compiled kg facility; VELITE_WIRE_HAS_KG lights the
    // factory from the same header this gate checks).
    Add(&v, "kg/crud");
    Add(&v, "kg/wire-content-addressed-facts");
    Add(&v, "kg/wire-fact-content-addressed");
    Add(&v, "kg/wire-fact-sig-verify");
    Add(&v, "kg/wire-prototype-frozen");
    Add(&v, "kg/wire-restore-preserves-prototype");
    Add(&v, "kg/wire-kgnode-reserved-messages");
    Add(&v, "kg/wire-methodmetadata-direct-invocation");
    Add(&v, "kg/wire-methodmetadata-vs-legion-update-disjoint");
    Add(&v, "kg/wire-subscription-replay");
    Add(&v, "kg/wire-search-hook");          // disposition-gated, §3.3c-4
    Add(&v, "kg/declarative-type-creation");  // disposition-gated, §3.3c-4
    Add(&v, "kg/typed-handle-prototype");
    Add(&v, "kg/wire-query");
    // --- query (the legion-query engine rides the kg facility).
    Add(&v, "query/subsumption-typed");
    Add(&v, "query/grammar");
    Add(&v, "query/projection-capability");
    Add(&v, "query/update-atomic");
    Add(&v, "query/wire-successor-specification");
    // --- provenance (fact provenance rides the kg fact store).
    Add(&v, "provenance/wire-fact-provenance-fields");
    Add(&v, "provenance/wire-freshness-modes");
    Add(&v, "provenance/wire-identifies-relation");  // §3.3c-5
    // --- subscribe (the vendored KG wire-streaming trio; the chrome event
    // surface joins at CF-5).
    Add(&v, "subscribe/wire-streaming");
#endif  // AURELIAN_CLAIMS_HAS_KG
#if defined(AURELIAN_CLAIMS_HAS_ONTOLOGY)
    // --- ontology (TypeRegistry + ontology factory, compiled in).
    Add(&v, "ontology/subsumption");
    Add(&v, "ontology/wire-type-uri-resolvable");
    Add(&v, "ontology/wire-canonical-types-present");
    Add(&v, "ontology/wire-relations-resolvable");
    Add(&v, "ontology/wire-procedural-relations-present");
    Add(&v, "ontology/wire-relation-domain-range-enforced");
    Add(&v, "ontology/wire-entity-prototype-is-type");
    Add(&v, "ontology/wire-dag-except-root-selfloops");
    Add(&v, "ontology/wire-prototype-frozen-at-construction");
    Add(&v, "ontology/typehandle-singleton");
#endif  // AURELIAN_CLAIMS_HAS_ONTOLOGY
    // --- discovery (the slot-0 resolver surface — §4.1's one mount table).
    Add(&v, "discovery/wire-catalog");
    Add(&v, "discovery/catalog");
    Add(&v, "discovery/wire-get");
    Add(&v, "discovery/wire-getResource");
    Add(&v, "discovery/wire-bootstrap-handle-canonical-surface");
    Add(&v, "discovery/wire-one-resolver-slot-0");
    Add(&v, "discovery/no-ambient-discovery");
    // --- observability.
    Add(&v, "observability/wire-audit-log-entries");
    Add(&v, "observability/wire-canonical-span-names");
    Add(&v, "observability/wire-discipline-bundle");
    Add(&v, "observability/span-emission");
    Add(&v, "observability/audit-event-extension");
#if defined(AURELIAN_CLAIMS_HAS_RESOURCES)
    // --- resources (the acquisition/materialiser surface CF-7 proves; the
    // publish/install store family stays queued CF-EXT + capped).
    Add(&v, "resources/materialiser");
    Add(&v, "resources/wire-materializer-typed");
    Add(&v, "resources/wire-dependency-walk");
    Add(&v, "resources/manifest-parser-inline");
#endif  // AURELIAN_CLAIMS_HAS_RESOURCES
    // --- local-resources (the vendored intrinsic constructor mount +
    // FilesystemHandle, compiled with the wire lib itself; §3.3b/R4-F6c).
    // wire-credential-containment is OUT-OF-SCOPE and wire-resource-
    // manifest-declarative-only NOT CLAIMED (verified upstream absences,
    // §3.3c-10/-12) — neither enters the manifest.
    Add(&v, "local-resources/wire-freshness-metadata");
    Add(&v, "local-resources/wire-schema-declaration");
    Add(&v, "local-resources/wire-capability-scoped-getresource");
    Add(&v, "local-resources/wire-native-error-mapping");
#if defined(AURELIAN_CLAIMS_HAS_KG)
    Add(&v, "local-resources/wire-resource-queryable-as-kg-entity");
#endif
    // --- federation (the registry surface L0-core 05 drives; the cross-org
    // sub-facets stay queued CF-L2 + capped).
    Add(&v, "federation/peer-registry");
    Add(&v, "federation");
    // --- embodiment (the kept membrane rows + the rows this design
    // discharges).
    Add(&v, "embodiment/wire-install-membrane");
    Add(&v, "embodiment/wire-one-shot-consumption");
    Add(&v, "embodiment/wire-single-network-endpoint");
    Add(&v, "embodiment/wire-direct-mount-unreachable-after-install");
    Add(&v, "embodiment/wire-registered-mount-forwards");
    Add(&v, "embodiment/wire-federated-mount-via-update");
    return v;
  }());
  return *claimed;
}

const std::vector<std::string>& aurelian_claimed_internal_facets() {
  static const base::NoDestructor<std::vector<std::string>> claimed([] {
    std::vector<std::string> v;
    Add(&v, "embodiment/internal-reflective-mirror");  // kept (ACM-10)
    Add(&v, "agentspace/internal-single-threaded-dispatch");
    Add(&v, "agentspace/internal-audit-log");
    Add(&v, "embodiment/internal-operator-config-typed");
#if defined(AURELIAN_CLAIMS_HAS_ONTOLOGY)
    Add(&v, "ontology/internal-bootstrap-types");
    Add(&v, "ontology/internal-no-parallel-subsumption-index");
    Add(&v, "ontology/internal-procedural-relation-subsumption");
#endif
#if defined(AURELIAN_CLAIMS_HAS_KG)
    Add(&v, "kg/internal-subscription-index");
#endif
    return v;
  }());
  return *claimed;
}

}  // namespace aurelian
