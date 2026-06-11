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

class UdsRegister {
 public:
  UdsRegister();
  ~UdsRegister();

  UdsRegister(const UdsRegister&) = delete;
  UdsRegister& operator=(const UdsRegister&) = delete;

  // Dial Agrippa's registration UDS at `socket_path`, serve `facet`
  // (e.g. "chrome") presenting `child_dest_hash` as the audit principal, and
  // send the update{Mount} register frame. `dispatch` resolves forwarded leaf
  // asks. Returns true iff the dial connected (the register frame was sent).
  // Spawns a serve thread that pumps inbound forwarded asks until Stop().
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
             const std::string& child_dest_hash,
             ChromeDispatchFn dispatch,
             const std::vector<uint8_t>& cap_anchor);

  // Stop the serve loop, close the connection (the hub revokes the facet), join.
  void Stop();

  bool connected() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace aurelian

#endif  // AURELIAN_FEDERATION_UDS_REGISTER_H_
