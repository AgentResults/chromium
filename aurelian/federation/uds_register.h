// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C9 — register the `chrome` facet INTO Agrippa over the local UDS
// (AURELIAN-DESIGN.md §3.6/§13, the SHIPPED machine-federation pattern). This
// is the canonical, de-socketed path — the ONE production bring-up (the
// C9-era WSS listener scaffold went at ACM-R(3), design section 7's
// DELETE-default row): Aurelian opens NO inbound network socket. It
// DIALS Agrippa's generation-stable registration UDS (~/.legion/agrippa.sock),
// runs a substrate wire::Dispatcher whose slot-0 bootstrap resolves the sealed
// legion://chrome/ root, and sends the register handshake
//   update{kind:"Mount", name:"chrome",
//          type:"legion://types/RegisteredEmbodimentHandle", params:{childDestHash}}
// — the same UdsDispatcherLink::send_register frame Frontinus uses, routed by
// Agrippa's ChildEdge into register_embodiment. The hub then forwards a
// controller's leaf asks back over the UDS as dispatchAt{uri,verb,spec}.
//
// Velite types are pimpl-hidden so Chrome/test TUs stay Velite-free.

#ifndef AURELIAN_FEDERATION_UDS_REGISTER_H_
#define AURELIAN_FEDERATION_UDS_REGISTER_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace aurelian {

// Resolves a slash-path leaf ask against the legion://chrome/ root and returns
// the serialized reply (e.g. "system/info", "__getIdentity"). HS-3 (ACM-2w,
// design section 5): the caller's spec crosses this seam SERIALIZED (canonical
// JSON; empty string = no spec) and VALUE-ONLY — the seam refuses slot-ref-
// bearing specs typed rather than silently flattening designation; the
// serialized form keeps this header velite-free per the pimpl discipline.
// Production wires the bootstrap's exported dispatch; tests pass a fake.
using ChromeDispatchFn = std::function<std::string(
    const std::string& path, const std::string& serialized_spec)>;

// CF-6 (design §6.1) — F8b: the ONE exported dispatch root changes
// signature, named + inventoried. The begin-form is record-creating and
// NON-BLOCKING: it creates the HS-1 completion record on the calling
// (serve) thread PRE-POST (round-6: every record Stop()-flippable), posts
// the UI dispatch, and returns the record — the caller wraps it in a
// PendingDispatchHandle and the dispatcher's deferred-pending pump settles
// it (N>1 in-flight per connection is then structural). The HS-3 typed
// slot-ref refusal still runs synchronously on the serve thread BEFORE the
// spec is serialized into this seam — a refusal never reaches the UI
// thread. The synchronous ChromeDispatchFn above retires from the wire
// path with the blocking bridge (design §6.2); it remains the in-process
// boot/test shape only.
struct CompletionRecord;
using BeginChromeDispatchFn =
    std::function<std::shared_ptr<CompletionRecord>(
        const std::string& path, const std::string& serialized_spec)>;

// CF-5 — the wire-subscribe registration seam (design §5.2): called on the
// serve thread for a `legion-subscribe-remote` naming a chrome event-node
// URI; the installed implementation constructs the producer-side
// WireSinkHandle (designation never crosses a serialized seam — the sub_id
// relay, design §5.4), posts the CdpSessionRegistry registration to the UI
// thread (the HS-1 posted-task pattern), and returns the serialized ack
// ("subscribed:<sub_id>" / "broken:…") through the blocking HS-1 settle
// (one-in-flight until CF-6). Events flow back through `mailbox` — the
// serve loop drains it on the serve thread. Velite-free by construction
// (the mailbox is opaque here).
class WireEventMailbox;
using ChromeWireSubscribeFn =
    std::function<std::shared_ptr<CompletionRecord>(
        const std::string& uri, const std::string& sub_id,
        WireEventMailbox* mailbox)>;

class UdsRegister {
 public:
  UdsRegister();
  ~UdsRegister();

  UdsRegister(const UdsRegister&) = delete;
  UdsRegister& operator=(const UdsRegister&) = delete;

  // Dial Agrippa's registration UDS at `socket_path`, serve `facet`
  // (e.g. "chrome"), and send the update{Mount} register frame presenting
  // the browser's REAL destHash — derived from `peer_seed` via the seeded
  // vendored identity ([EMBODIMENT-REGISTERED-CHILD-OWN-IDENTITY], CF-4;
  // the caller persists the seed so the destHash is stable across
  // restarts). The slot-0 surface is the UNIFIED bootstrap (the vendored
  // ConformanceBootstrap with chrome mounted + the claims cap) composed
  // with the thin dispatchAt facade (design §4.2) — the same surface the
  // conformance serve exhibits — and the computed __handshake manifest is
  // emitted after the register ask (facets.md §6a). `dispatch` resolves
  // forwarded leaf asks. Returns true iff the dial connected (the register
  // frame + handshake were sent). Spawns a serve thread that pumps inbound
  // forwarded asks until Stop().
  //
  // ACM-8 (design section 5 prove-or-build): `cap_anchor` is the operator/
  // machine cap trust anchor (32-byte Ed25519 pub — the SAME anchor the
  // renderer membranes are provisioned with at boot). A dispatchAt frame
  // carrying a `cap` field (a serialized delegation chain, cap_wire format)
  // is verified back to this anchor and its effective predicate enforced on
  // the full target URI BEFORE the dispatch runs — descendant-scoped
  // attenuation at the seam, which the proven path otherwise lacks (Agrippa's
  // gate is mint-time, facet-granularity; the hub forwards per-dispatch
  // blindly). Empty/wrong-sized anchor = no anchor provisioned: a presented
  // cap then fail-closes typed (cap-untrusted-anchor); capless dispatches
  // keep the connection's facet-level authority either way (the status quo —
  // the Agrippa mint gate authorized the registration).
  bool Start(const std::string& socket_path,
             const std::string& facet,
             const std::string& peer_seed,
             BeginChromeDispatchFn dispatch,
             const std::vector<uint8_t>& cap_anchor,
             ChromeWireSubscribeFn wire_subscribe = {});

  // Stop the serve loop, close the connection (the hub revokes the facet), join.
  void Stop();

  bool connected() const;

  // The registered identity (32-hex destHash) — set by Start().
  const std::string& dest_hash() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace aurelian

#endif  // AURELIAN_FEDERATION_UDS_REGISTER_H_
