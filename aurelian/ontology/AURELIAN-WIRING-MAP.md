# Aurelian wiring map (GENERATED — do not edit)

Regenerate: `python3 aurelian/ontology/surface_coverage_audit.py --src-root . --ontology aurelian/ontology/aurelian-source-domain.ontology.yaml --write-map aurelian/ontology/AURELIAN-WIRING-MAP.md`

## Mounted roots (legion://chrome/<name>)

| root | kind | projector | factory | host structure | types stamped | files |
|---|---|---|---|---|---|---|
| cdp | projector | CdpMirror | CreateCdpMirror | cdp-descriptor | legion://types/CdpCatalog<br>legion://types/CdpCommand<br>legion://types/CdpDomain<br>legion://types/CdpEvent | mirror/cdp_mirror.cc<br>mirror/cdp_mirror.h<br>mirror/cdp_session.cc<br>mirror/cdp_session.h<br>mirror/cdp_agent_client.cc<br>mirror/cdp_agent_client.h<br>mirror/value_convert.cc<br>mirror/value_convert.h |
| targets | projector | TargetsMirror | CreateTargetsMirror | targets | legion://types/ChromeTarget<br>legion://types/ChromeTargets | mirror/targets_mirror.cc<br>mirror/targets_mirror.h |
| prefs | projector | PrefsMirror | CreatePrefsMirror | prefs-registry | legion://types/ChromePreference<br>legion://types/ChromePreferences | mirror/prefs_mirror.cc<br>mirror/prefs_mirror.h |
| services | projector | ServicesMirror | CreateServicesMirror | keyed-services-graph | legion://types/ChromeService<br>legion://types/ChromeServices | mirror/services_mirror.cc<br>mirror/services_mirror.h |
| system | facade | — | — | — | — | handles/root/root_handle.cc<br>handles/browser/system_handle.cc<br>handles/browser/system_handle.h |
| tabs | facade | — | — | — | — | handles/root/root_handle.cc<br>handles/browser/tabs_overview.cc<br>handles/browser/tabs_overview.h<br>handles/browser/tabstrip_handle.cc<br>handles/browser/tabstrip_handle.h |
| gpu | facade | — | — | — | — | handles/root/root_handle.cc<br>handles/browser/gpu_handle.cc<br>handles/browser/gpu_handle.h |
| media | facade | — | — | — | — | handles/media/media_handles.cc<br>handles/media/media_handles.h |

## Layer-1 types -> IDENTIFIES -> Layer-3 canonical

| type | identifies | stamped by |
|---|---|---|
| legion://types/CdpCatalog | legion://types/Catalog | mirror/cdp_mirror.cc |
| legion://types/CdpDomain | legion://types/Catalog | mirror/cdp_mirror.cc |
| legion://types/CdpCommand | legion://types/Operation | mirror/cdp_mirror.cc |
| legion://types/CdpEvent | legion://types/Operation | mirror/cdp_mirror.cc |
| legion://types/ChromeTargets | legion://types/Catalog | mirror/targets_mirror.cc |
| legion://types/ChromeTarget | legion://types/Service | mirror/targets_mirror.cc |
| legion://types/ChromePreferences | legion://types/Catalog | mirror/prefs_mirror.cc |
| legion://types/ChromePreference | legion://types/Setting | mirror/prefs_mirror.cc |
| legion://types/ChromeServices | legion://types/Catalog | mirror/services_mirror.cc |
| legion://types/ChromeService | legion://types/Service | mirror/services_mirror.cc |

## Host correspondence (design section 6 — host-side)

| family | status | projector | reason/note |
|---|---|---|---|
| cdp-descriptor | mapped | CdpMirror | — |
| prefs-registry | mapped | PrefsMirror | — |
| targets | mapped | TargetsMirror | — |
| keyed-services-graph | mapped | ServicesMirror | catalog-only — invoke is the typed no-invoke-surface refusal (design section 2 tier 2) |
| ax-tree | covered-via | CdpMirror | Accessibility.* + DOM.*/Input.* inside the CDP mirror — no separate projector, no AXControl stamp |
| features-flags-registry | out-of-scope | — | base::Feature/FeatureList enumerates compile-time flags; mutation needs process restart semantics the mirror does not model in v1 |
| mojo-interface-broker | out-of-scope | — | the broker binds typed mojo pipes, not a reflective catalog; no schema-carrying enumeration surface exists to project in v1 |
| command-line-switches | out-of-scope | — | process-launch-frozen key/value strings; read-only after boot and untyped — no live control surface to mirror in v1 |
| histograms-registry | out-of-scope | — | telemetry observation, not browser control; volume (tens of thousands of histograms) without verbs makes it catalog noise in v1 |
| extensions-registry | out-of-scope | — | the bespoke extensions_handle was DELETED at ACM-R (operations territory is the chrome-layer Extensions.* domain); the registry itself stays unprojected in v1 — CDP Extensions.* covers operations, not reflective enumeration — revisit per roll |
