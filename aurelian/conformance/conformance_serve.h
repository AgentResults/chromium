// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// CF-1 — the conformance serve mode (conformant-federation design §2.1/§4.1).
// Under --aurelian-conformance-serve=<uds-path> the browser, after the normal
// install-membrane seal, CONNECTS OUT to the named UDS (no inbound listener,
// [EMBODIMENT-SINGLE-NETWORK-ENDPOINT]) and serves the unified vendored
// slot-0 surface over raw NDJSON lines — the launcher
// (aurelian-conformance-peer) is a zero-frame-aware byte pump bridging the
// canonical runner's stdio to this socket, so the UDS carries NDJSON, never
// the envelope framing the register-in wire uses. The serve:
//   - builds the seeded vendored ConformanceBootstrap (VELITE_PEER_SEED via
//     the vendored env path — the launcher forwards the runner's per-peer
//     seed untouched),
//   - mounts legion://chrome (the ONE NavHandle root over the ONE exported
//     ChromeDispatchFn) pre-seal through mount_resource — the Marius
//     embodiment-install primitive — then seal()s,
//   - applies the claims cap and emits the COMPUTED __handshake manifest
//     (facet_claims.h — one claims set, two projections, design §3.2),
//   - pumps frames until Stop() (harness-owned shutdown) or disconnect
//     (launcher lane: `on_disconnect` runs once — exit-with-the-connection).
//
// Velite types are pimpl-hidden so Chrome TUs stay velite-free.

#ifndef AURELIAN_CONFORMANCE_CONFORMANCE_SERVE_H_
#define AURELIAN_CONFORMANCE_CONFORMANCE_SERVE_H_

#include <memory>
#include <string>

#include "aurelian/federation/uds_register.h"
#include "base/functional/callback.h"

namespace aurelian {

// The conformance-mode connectivity switch (a path the serve reads — the
// VELITE_OPERATOR_CONFIG doctrine, never a behaviour flag).
inline constexpr char kConformanceServeSwitch[] = "aurelian-conformance-serve";

class ConformanceServe {
 public:
  ConformanceServe();
  ~ConformanceServe();

  ConformanceServe(const ConformanceServe&) = delete;
  ConformanceServe& operator=(const ConformanceServe&) = delete;

  // Connect out to the UDS at `socket_path` and serve. `dispatch` resolves
  // chrome-mount leaf asks (the ONE exported ChromeDispatchFn).
  // `on_disconnect` runs at most once, on the serve thread, when the
  // connection ends while Stop() has NOT been initiated — the launcher-lane
  // exit path ("browser lifetime ≡ connection lifetime in conformance
  // mode"); under the browsertest harness the fixture holds the connection
  // and Stop() flips first, so it never runs. Returns true iff connected
  // and the handshake was emitted.
  bool Start(const std::string& socket_path,
             ChromeDispatchFn dispatch,
             base::OnceClosure on_disconnect);

  // Harness-owned shutdown: flip stopping, join the serve thread, close.
  // The serve never initiates browser exit after Stop().
  void Stop();

  bool connected() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace aurelian

#endif  // AURELIAN_CONFORMANCE_CONFORMANCE_SERVE_H_
