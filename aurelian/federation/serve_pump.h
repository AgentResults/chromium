// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// CF-4 — ONE serve loop, two callers (conformant-federation design §4.1,
// closing the recorded CF-1 deviation): the register-in (envelope-framed
// UdsChannel) and the conformance serve (raw NDJSON lines) run the SAME
// drain → dispatch → pump → idle shape, parameterised only by the
// transport drain. The loop exits on Stop() or session close — after a
// session-scoped CLOSE nothing is pumped or emitted
// ([PROTOCOL-SESSION-TERMINATION-EQUALS-CLOSE], L0-core 07).
//
// Velite-typed INTERNAL header (the nav_handle_internal.h discipline).

#ifndef AURELIAN_FEDERATION_SERVE_PUMP_H_
#define AURELIAN_FEDERATION_SERVE_PUMP_H_

#include <atomic>
#include <functional>

#include "velite/agentspaces-wire/dispatcher.hpp"

namespace aurelian {

// What one drain pass observed.
enum class ServeDrain {
  kProgress,  // frames were fed to the dispatcher
  kIdle,      // nothing inbound this pass
  kEnded,     // transport gone (EOF / hard error) — the loop returns
};

// Returns true iff the loop ended because the drain reported kEnded (the
// caller's end-of-serve policy: the conformance lane ends on EOF OR on a
// dispatched session CLOSE — connection ≡ lifetime; the register-in lane
// ends only on channel close/EOF and otherwise stays RESIDENT, draining the
// channel — wire hygiene: a closed dispatcher still needs its transport
// read so a refused oversize body keeps discarding instead of wedging the
// sender). False on Stop(). Never pumps after a session-scoped CLOSE.
// `flush` (optional) runs after the pump while the session is open — the
// CF-5 wire-event mailbox drain rides here (emission stays single-threaded
// on the serve thread, the ONE dispatcher emission path).
inline bool RunServeLoop(const std::atomic<bool>& stop,
                         velite::agentspaces::wire::Dispatcher& disp,
                         const std::function<ServeDrain()>& drain,
                         const std::function<void()>& idle,
                         const std::function<void()>& flush = {}) {
  while (!stop.load()) {
    const ServeDrain d = drain();
    if (d == ServeDrain::kEnded) {
      return true;
    }
    if (!disp.closed()) {
      disp.pump_pending_answers();
      if (flush) {
        flush();
      }
    }
    if (d == ServeDrain::kIdle) {
      idle();
    }
  }
  return false;
}

}  // namespace aurelian

#endif  // AURELIAN_FEDERATION_SERVE_PUMP_H_
