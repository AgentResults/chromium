# Aurelian — the bespoke replacement inventory (ACM-R, HS-2)

The authored inventory `AURELIAN-GENERIC-CONTROL-DESIGN.md` section 7 demands:
every `.cc`/`.h` file under `aurelian/handles/**` (recursive), the federation
bring-up set (`federation/uds_register*`, `federation/wss_peer*`,
`federation/nav_handle_internal*`, `federation/pending_dispatch*`,
`federation/serve_pump*`, `federation/wire_event*`),
`capability/cap_gated_cookies*`, the
renderer Mojo bridge (`renderer/**`), the virtual-A/V layer (`media/**`) and
the conformance layer (`conformance/**`, CF-1) carries exactly ONE
disposition row.
`inventory/no_parallel_bespoke_audit.py` (a build action gating
`aurelian_root`, like the ACM-7 audits) fails the build if (a) an in-scope
file has no row or two rows, (b) a DELETE-row file still exists, (b2) a
non-DELETE row names a ghost, (c) a facade verb in `root_handle.cc` shadows
the mirror without a FOLD-verb row, or (d) a row names a path outside the
audit's own scope. Anything less ships a parallel system.

Dispositions: `DELETE` (file gone — territory named), `FOLD` (facade verb
kept as a semantic convenience; implementation delegates to the mirror),
`KEEP-infrastructure` (not browser-control surface; the mirror rides or
ignores it), `KEEP-test-harness` (production-shaped code surviving ONLY as
test harness — Phase-A review F3), `OUT-OF-SCOPE` (latent file recorded,
neither mirrored nor part of the contract surface).

## Rows

| files | disposition | reason |
|---|---|---|
| handles/browser/accessibility_handle.cc handles/browser/accessibility_handle.h handles/browser/accessibility_handle_browsertest.cc | DELETE | C7.a orphan (own-test-only since C0 sealed the root); territory = Accessibility.* + DOM.*/Input.* through the mirror — keystone leg (d) asserted it federated |
| handles/browser/audio_handle_browsertest.cc | DELETE | tested tab_handle's orphaned tab-audio mute/audibility class; no CDP tab-audio-mute ground in v1 (recorded; revisit per roll) |
| handles/browser/bookmarks_handle.cc handles/browser/bookmarks_handle.h handles/browser/bookmarks_handle_browsertest.cc | DELETE | C13.a orphan (design section-7 authored row); no CDP bookmarks ground in v1 (recorded; revisit per roll) |
| handles/browser/clipboard_handle.cc handles/browser/clipboard_handle.h handles/browser/clipboard_handle_browsertest.cc | DELETE | C13.b orphan (authored row); clipboard interaction rides Input.*/Runtime.evaluate through the mirror where needed |
| handles/browser/devtools_handle.cc handles/browser/devtools_handle.h | KEEP-test-harness | F3: SendCdpCommand is the ACM-S2 raw-session live-pin harness — it arbitrates the derived session-context table against the HOST without the mirror in the loop, so it can never migrate; test-only linkage (aurelian_browsertests). CaptureCdpEvent was deleted at ACM-4 when its last caller migrated |
| handles/browser/devtools_handle_browsertest.cc | KEEP-test-harness | the ACM-S1 conformance pins, migrated onto the mirror at ACM-3; its stale devtools_handle.h include went in the ACM-R F6c sweep |
| handles/browser/dialogs_handle.cc handles/browser/dialogs_handle.h handles/browser/dialogs_handle_browsertest.cc | DELETE | C11.b orphan; territory = Page.javascriptDialogOpening / Page.handleJavaScriptDialog |
| handles/browser/emulation_handle.cc handles/browser/emulation_handle.h handles/browser/emulation_handle_browsertest.cc | DELETE | C11.a orphan; territory = Emulation.* (the keystone viewport pin already drives Emulation.setDeviceMetricsOverride through the mirror) |
| handles/browser/extensions_handle.cc handles/browser/extensions_handle.h handles/browser/extensions_handle_browsertest.cc | DELETE | C7.b orphan; operations territory = the chrome-layer Extensions.* domain; the extensions REGISTRY remains a recorded out-of-scope correspondence row (reason updated in the deleting commit) |
| handles/browser/find_glue.cc handles/browser/find_glue.h handles/browser/find_handle_browsertest.cc | DELETE | find-in-page observer glue; its last consumer was tab_handle's orphaned find class; no CDP find-in-page domain in v1 (recorded; revisit per roll) |
| handles/browser/frame_handle_browsertest.cc | DELETE | tested tab_handle's orphaned Mojo frame-dispatch HANDLE surface; territory = Runtime.evaluate / DOM.* through the mirror (the renderer Mojo BRIDGE itself is a KEEP row — C-MEDIA) |
| handles/browser/frames_handle.cc handles/browser/frames_handle.h handles/browser/frames_handle_browsertest.cc | DELETE | C2.x orphan; territory = Page.getFrameTree |
| handles/browser/gpu_handle.cc handles/browser/gpu_handle.h handles/browser/gpu_handle_browsertest.cc | DELETE | support of the gpu facade, repointed at ACM-R: gpu/info reshapes SystemInfo.getInfo through the mirror session (FOLD verbs below); the bespoke GpuDataManager read went with it |
| handles/browser/history_handle.cc handles/browser/history_handle.h handles/browser/history_handle_browsertest.cc | DELETE | C13.e orphan (authored row); no CDP profile-history ground in v1 (recorded; revisit per roll) |
| handles/browser/input_handle_browsertest.cc | DELETE | tested tab_handle's orphaned input synthesis; territory = Input.* through the mirror — keystone leg (d) (AX find → DOM.getBoxModel → Input click) asserted it federated |
| handles/browser/nav_history_handle.cc handles/browser/nav_history_handle.h handles/browser/nav_history_handle_browsertest.cc | DELETE | C1.z orphan; territory = Page.getNavigationHistory |
| handles/browser/page_info_handle.cc handles/browser/page_info_handle.h handles/browser/page_info_handle_browsertest.cc | DELETE | C11.c orphan (authored row); MIME/encoding ride the mirror's Network/Page response metadata |
| handles/browser/permissions_handle.cc handles/browser/permissions_handle.h handles/browser/permissions_handle_browsertest.cc | DELETE | C7.c orphan; territory = Browser.grantPermissions / Browser.setPermission (browser session) |
| handles/browser/print_handle_browsertest.cc | DELETE | tested tab_handle's orphaned print class; territory = Page.printToPDF |
| handles/browser/process_handle.cc handles/browser/process_handle.h handles/browser/process_handle_browsertest.cc | DELETE | C12.a orphan; territory = SystemInfo.getProcessInfo |
| handles/browser/profile_handle.cc handles/browser/profile_handle.h handles/browser/profile_handle_browsertest.cc | DELETE | C13.d orphan; incognito/identity bits unprojected in v1 (recorded) — no parallel surface survives |
| handles/browser/screenshot_handle_browsertest.cc | DELETE | tested tab_handle's orphaned screenshot class; territory = Page.captureScreenshot — keystone leg (e) asserted the serialized wire-frame window federated |
| handles/browser/system_handle.cc handles/browser/system_handle.h handles/browser/system_handle_browsertest.cc | DELETE | support of the system facade, repointed at ACM-R: system/info reshapes SystemInfo.getProcessInfo through the mirror session (FOLD verbs below) |
| handles/browser/tab_handle.cc handles/browser/tab_handle.h handles/browser/tab_handle_browsertest.cc | DELETE | the design-F8 row — NOT a cheap delete: carried the bootstrap refactor (browser_main_extra.cc keeps the tab-lifecycle observers + CapAnchorProvisioner provisioning + the WebContents-id dedupe; TabHandleImpl/TabsHandleImpl, the nav/input/screenshot/print/find/zoom/audio/frame classes and the unreachable legion://chrome/browser/ registry went). Per-tab territory = targets/<id>/cdp/... sub-mirrors |
| handles/browser/tabgroups_handle.cc handles/browser/tabgroups_handle.h handles/browser/tabgroups_handle_browsertest.cc | DELETE | C13.c orphan (authored row); no CDP tab-group ground in v1 (recorded; revisit per roll) |
| handles/browser/tabs_overview.cc handles/browser/tabs_overview.h | DELETE | support of the tabs facade, repointed at ACM-R: count/activeUrl serve through the mirror + the recorded selection glue (FOLD verbs below) |
| handles/browser/tabstrip_handle.cc handles/browser/tabstrip_handle.h handles/browser/tabstrip_handle_browsertest.cc | DELETE | support of the tabs facade, repointed at ACM-R: open/activate/close = Target.createTarget / Page.bringToFront / Target.closeTarget through the mirror sessions; strip-index resolution + the last-tab guard survive as recorded glue in root_handle.cc |
| handles/browser/windows_handle.cc handles/browser/windows_handle.h handles/browser/windows_handle_browsertest.cc | DELETE | C1.y orphan; territory = Browser.getWindowBounds / Browser.setWindowBounds |
| handles/browser/zoom_handle_browsertest.cc | DELETE | tested tab_handle's orphaned zoom class; no CDP user-zoom ground in v1 (Emulation.setPageScaleFactor is emulation, not user zoom — recorded; revisit per roll) |
| handles/media/media_seam.cc handles/media/media_seam.h handles/media/media_seam_unittest.cc | KEEP-infrastructure | the process-global frame/audio seam (base-only; the content capture device and the Velite media handles both read it) — design section 12.5, orthogonal to the mirror |
| handles/media/media_handles.cc handles/media/media_handles.h | KEEP-infrastructure | the bindable media surface (video_sink / audio_sink / audio_source) — the `media` facade; CDP has no virtual-A/V ground |
| handles/network/network_handle.cc handles/network/network_handle.h handles/network/network_handle_browsertest.cc | DELETE | C4 bespoke: cookies/intercept/observation territory = Network.* / Fetch.* / Storage.* through the mirror; its only production consumer was the chrome_content_browser_client.cc throttle hook (default pass-through, rule mutators unreachable in production) — hook + BUILD dep removed in the same commit (fork-delta shrink) |
| handles/profile/audio_capture_handle.cc handles/profile/audio_capture_handle.h handles/profile/audio_capture_handle_browsertest.cc | KEEP-infrastructure | production consumer is the HOST's chrome-layer Asmodeus CDP domain (asmodeus_handler.cc CaptureTabAudio/Stop/Level) — the mirror serves Asmodeus.* THROUGH it; implementation substrate, not parallel surface |
| handles/profile/credentials_handle.cc handles/profile/credentials_handle.h handles/profile/credentials_handle_unittest.cc | OUT-OF-SCOPE | latent, never wired into the reachable graph (no root mount, no production caller); revisit under the security track (the design section-7 authored example) |
| handles/root/root_handle.cc handles/root/root_handle.h | KEEP-infrastructure | the sealed root: the mirror mounts, the FOLD facade home (delegating verbs per the FOLD table), RootDispatch/DispatchOutcome (HS-1) and the one-root construction counter (ACM-2w) |
| handles/root/root_handle_browsertest.cc handles/root/tab_reach_browsertest.cc | KEEP-infrastructure | the root/facade reach + no-regression pins (migrated onto pending-settle waits when the facades repointed) |
| handles/root/wire_serialize.cc handles/root/wire_serialize.h handles/root/wire_serialize_unittest.cc | KEEP-infrastructure | THE one wire serializer (design section 3) |
| handles/streams/subscription_producer.cc handles/streams/subscription_producer.h handles/streams/subscription_producer_unittest.cc | KEEP-infrastructure | the substrate subscription transport ACM-4 consumes (CDP event fan-out) |
| handles/streams/mojo_stream_bridge.cc handles/streams/mojo_stream_bridge.h handles/streams/stream_subscription_browsertest.cc | KEEP-infrastructure | the renderer Mojo stream bridge (C6.b; C-MEDIA rides it) |
| federation/uds_register.cc federation/uds_register.h federation/uds_register_unittest.cc | KEEP-infrastructure | TWO bring-up modes, ONE surface, ONE serve loop (CF-4): the C9 register-in and the conformance serve both serve the unified bootstrap composed with the AurelianSlotZero wrapper (the dispatchAt FOLD facade + ACM-8 cap-gate seam; the four-verb AurelianBootstrap shim DELETED at CF-4 — its wire assertions migrated to the wrapper unchanged); real persisted destHash + computed __handshake on the register wire; unit tests. CF-8 (design §7) — the ONE-authorization-model seam list, all gating through GateFederationDispatch against the one operator anchor (audit rule (e) locks the caller set): the dispatchAt facade, getResource acquisition (presented cap → handle born bound), bound NavHandle per-ask re-gate (`now` re-evaluated; child mint AND-only parent ∧ presented), and the wire subscribe (gated before registration; chain min-expiry feeds the mailbox's per-delivery re-check) |
| federation/nav_handle_internal.h | KEEP-infrastructure | CF-1/CF-4: the ONE NavHandle factory seam + the unified slot-0 wrapper factory (MakeUnifiedSlotZero) — velite-typed, internal; implementations stay in uds_register.cc, no parallel handle |
| federation/serve_pump.h | KEEP-infrastructure | CF-4 (design §4.1, closing the CF-1 deviation): ONE serve loop, two callers — drain→dispatch→pump→idle parameterised by the transport drain; never pumps after a session-scoped CLOSE; CF-5 adds the optional flush hook (the wire-event mailbox drain) |
| federation/wire_event_mailbox.cc federation/wire_event_mailbox.h | KEEP-infrastructure | CF-5 (design §5.2): the ONE new wire-eventing mechanism — the mutex-guarded {sub_id,msg,event} mailbox (move-under-the-lock ownership handoff; per-subscription cap, typed SubscriptionOverflow termination) + the producer-constructed WireSinkHandle (SubSinkHandle's shape, enqueue instead of inline emit_tell) |
| federation/pending_dispatch.cc federation/pending_dispatch.h | KEEP-infrastructure | CF-6 (design §6.1, F8b): the deferred-dispatch Handle over the UNCHANGED HS-1 CompletionRecord — kPending→Pending, kCompleted→ResolvedValue, kShutdown/deadline→typed Broken; the blocking bridge_dispatch glue DELETED with it (§6.2 DELETE-default: no production consumer remained; the Wait primitive stays in completion_bridge for Stop-coverage + unit pins) |
| federation/pending_dispatch_unittest.cc | KEEP-test-harness | CF-6 unit RED: record-state mapping + deadline-breaks-typed |
| federation/pending_dispatch_browsertest.cc | KEEP-test-harness | CF-6 RED: SecondAskSettlesWhileFirstInFlight — the one-in-flight lift pinned at wire-frame order |
| federation/nav_cap_browsertest.cc | KEEP-test-harness | CF-8 RED: the four AurelianNavCapBrowserTest cases (scoped cap binds the acquired handle; AND-only child inheritance; subscribe gated before registration; per-delivery cap-expiry termination) — real Ed25519 chains against the REAL sealed root |
| federation/wire_event_browsertest.cc | KEEP-test-harness | CF-5 RED/pin: CdpEventCrossesWireAsSubscriptionEventTell (the event-crosses-the-wire RED) + MailboxOverflowTerminatesTyped (the typed-bound pin, zero-cap deterministic) |
| conformance/unified_bootstrap.cc conformance/unified_bootstrap.h | KEEP-infrastructure | CF-4 (design §4.1): the ONE unified slot-0 bootstrap construction (seeded identity + chrome mount pre-seal + claims cap) — both bring-up modes build here, never two constructions that could drift |
| conformance/facet_claims.cc conformance/facet_claims.h | KEEP-infrastructure | CF-1 (design §3.2): the computed claims set — ONE source feeding the emitted __handshake manifest AND the bootstrap's claims cap, build-conditioned by the same guards as the surface |
| conformance/aurelian-claims-draft.yaml | KEEP-infrastructure | CF-1: the drafted matrix column block as DATA (CF-V classifier Input B; becomes the embodiments.yaml column at CF-10); lockstep with facet_claims asserted at CF-V |
| conformance/conformance_serve.cc conformance/conformance_serve.h | KEEP-infrastructure | CF-1 (design §2.1/§4.1): the conformance serve — connect-out NDJSON bring-up over the vendored bootstrap (chrome mounted, claims-capped); exit-with-the-connection in the launcher lane |
| conformance/aurelian-conformance-peer | KEEP-infrastructure | CF-1 (design §2.4): the impl-contributed launcher — stdio↔UDS byte pump, zero protocol logic (it cannot author a frame); spawned per launchPeer() by the canonical runner |
| conformance/select-claimed-vectors.mjs | KEEP-infrastructure | CF-V Step 1 (design §3.3a-D4/D4a): the mechanical lane selection + branch-3 classifier — Input A (banked handshake capture) ⊆ Input B (claims draft) asserted; every corpus sub-facet entry classified or exit≠0; glue like the launcher, no protocol logic |
| conformance/conformance_serve_browsertest.cc | KEEP-test-harness | CF-1 RED/GREEN: handshake-manifest equality (manifest ≡ computed claims), ping round-trip, unclaimed-facility typed decline — the browser-side serve pins |
| federation/wss_peer.cc federation/wss_peer.h federation/wss_peer_browsertest.cc | DELETE | the C9-era bring-up scaffold (design round-4 review N9): its only caller was its own browsertest; DELETE is the design default absent a named consumer — none was recorded. Its lazy second root already went at ACM-2w |
| capability/cap_gated_cookies.cc capability/cap_gated_cookies.h capability/cap_gated_cookies_browsertest.cc | DELETE | the design section-7 recorded decision: the membrane's reference uses are now the ACM-8 cap_gate (every federated dispatch) and the renderer membrane; gated cookie writes ride Network.setCookie / Storage.* under the same gate |
| renderer/aurelian_render_frame_observer.cc renderer/aurelian_render_frame_observer.h renderer/aurelian_wire_browsertest.cc | KEEP-infrastructure | the renderer-side Mojo bridge (C2/C3 infrastructure kept for C-MEDIA, design section 8) and the renderer cap-membrane consumer |
| media/aurelian_virtual_camera.cc media/aurelian_virtual_camera.h media/aurelian_virtual_camera_browsertest.cc | KEEP-infrastructure | the content-layer virtual camera (C-MEDIA-2): a real enumerable capture device pumping MediaSeam frames |
| media/aurelian_virtual_mic.cc media/aurelian_virtual_mic.h media/aurelian_virtual_mic_unittest.cc | KEEP-infrastructure | the TTS virtual microphone ring (C-MEDIA-2d/e) |

## FOLD verbs

The reachable facade verbs surviving as semantic conveniences (design
sections 1/4/7). Every `msg == "<verb>"` literal in `root_handle.cc` (minus
`__`-identity verbs and `describe`) must appear here, or the audit fails —
a new facade verb cannot ship without a recorded FOLD row.

| facade | verbs | delegation |
|---|---|---|
| system | info | SystemInfo.getProcessInfo through the browser-mirror session, reshaped {browserPid, rendererCount} at settle |
| gpu | info | SystemInfo.getInfo through the browser-mirror session, reshaped {vendorId, deviceId, glVendor, glRenderer} at settle |
| tabs | count activeUrl activeIndex open activate close url | count → Target.getTargets (reshaped count); open → Target.createTarget (settle-time strip-index lookup); activate → Page.bringToFront on THAT target's session; close → Target.closeTarget (the per-browser last-tab refusal stays a pre-dispatch guard); url/activeUrl → the live target's URL; activeIndex and index→target resolution are recorded strip/selection GLUE — host-UI concepts the descriptor does not model |
