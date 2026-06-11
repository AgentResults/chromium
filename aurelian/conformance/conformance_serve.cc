// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/conformance/conformance_serve.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <set>
#include <thread>
#include <utility>

#include "aurelian/conformance/facet_claims.h"
#include "aurelian/conformance/unified_bootstrap.h"
#include "aurelian/federation/nav_handle_internal.h"
#include "aurelian/federation/serve_pump.h"
#include "aurelian/federation/wire_event_mailbox.h"
#include "base/logging.h"
#include "velite/channel.hpp"
#include "velite/agentspaces-wire/conformance_bootstrap.hpp"
#include "velite/agentspaces-wire/dispatcher.hpp"
#include "velite/agentspaces-wire/peer_session.hpp"

namespace aurelian {

namespace {

using velite::agentspaces::ConformanceBootstrap;
using velite::agentspaces::wire::Dispatcher;

// Heap RX buffer at the wire MTU — the ACM-9p contract (heap-never-stack for
// raised buffers; the wss_peer 512 KiB-stack precedent stays honoured).
constexpr size_t kRxBuffer = velite::VELITE_CHANNEL_MAX_ENVELOPE_BYTES;

}  // namespace

struct ConformanceServe::Impl {
  int fd = -1;
  std::shared_ptr<ConformanceBootstrap> bootstrap;
  std::shared_ptr<Dispatcher> disp;
  // CF-5: the wire-event mailbox (UI producers, serve-thread drain).
  WireEventMailbox mailbox;
  std::thread serve_thread;
  std::atomic<bool> stop{false};
  std::atomic<bool> connected{false};
  base::OnceClosure on_disconnect;

  // Write one NDJSON line (frame + '\n'), whole or not at all. Serialized
  // by construction: the handshake is emitted before the serve thread
  // starts, and every later emission happens ON the serve thread (inbound
  // dispatch + pump both run there).
  bool SendLine(const std::string& frame) {
    std::string line = frame;
    line.push_back('\n');
    const char* p = line.data();
    size_t left = line.size();
    while (left > 0) {
      ssize_t n = ::send(fd, p, left, 0);
      if (n <= 0) {
        return false;
      }
      p += n;
      left -= static_cast<size_t>(n);
    }
    return true;
  }

  // The ONE serve loop (serve_pump.h, design §4.1) over the raw-NDJSON
  // line-reassembly drain. [PROTOCOL-SESSION-TERMINATION-EQUALS-CLOSE]
  // (scenario 07): the loop never pumps or emits after a session-scoped
  // CLOSE dispatches — the force-break is local on both sides.
  void Serve() {
    auto buf = std::make_unique<char[]>(kRxBuffer);
    std::string acc;
    RunServeLoop(
        stop, *disp,
        [this, &buf, &acc]() {
          struct pollfd pfd;
          pfd.fd = fd;
          pfd.events = POLLIN;
          pfd.revents = 0;
          const int pr = ::poll(&pfd, 1, 50);
          if (pr <= 0) {
            return ServeDrain::kIdle;
          }
          const ssize_t n = ::read(fd, buf.get(), kRxBuffer);
          if (n <= 0) {
            return ServeDrain::kEnded;  // EOF (launcher half-close) / error
          }
          acc.append(buf.get(), static_cast<size_t>(n));
          for (;;) {
            const size_t nl = acc.find('\n');
            if (nl == std::string::npos) {
              break;
            }
            std::string frame = acc.substr(0, nl);
            acc.erase(0, nl + 1);
            if (!frame.empty()) {
              disp->on_inbound(frame);
            }
            if (disp->closed()) {
              break;  // session over — nothing further dispatches
            }
          }
          // Conformance-lane policy: a dispatched session CLOSE ends the
          // serve (connection ≡ lifetime) — and nothing is pumped or
          // emitted after it ([PROTOCOL-SESSION-TERMINATION-EQUALS-CLOSE],
          // scenario 07).
          return disp->closed() ? ServeDrain::kEnded : ServeDrain::kProgress;
        },
        []() {},  // poll() is the idle wait
        // CF-5: the wire-event mailbox drain (the ONE emission path).
        [this]() {
          for (velite::agentspaces::Value& entry : mailbox.DrainAll()) {
            disp->emit_tell(0, std::string("legion-subscription-event"),
                            std::move(entry));
          }
        });
    // The connection/session ended while Stop() has not flipped:
    // launcher-lane semantics (design §2.3-07) — emit CLOSE{eof} if the
    // session is still open (the peer_main.cpp EOF behaviour; the frame
    // crosses the launcher's still-open read side), then
    // exit-with-the-connection.
    if (stop.load()) {
      return;  // harness-owned shutdown: never initiate exit
    }
    if (!disp->closed()) {
      disp->emit_close("eof");
    }
    connected.store(false);
    if (on_disconnect) {
      std::move(on_disconnect).Run();
    }
  }
};

ConformanceServe::ConformanceServe() : impl_(std::make_unique<Impl>()) {}

ConformanceServe::~ConformanceServe() {
  Stop();
}

bool ConformanceServe::Start(const std::string& socket_path,
                             BeginChromeDispatchFn dispatch,
                             const std::vector<uint8_t>& cap_anchor,
                             base::OnceClosure on_disconnect,
                             ChromeWireSubscribeFn wire_subscribe) {
  // Raw connect-out (the launcher listens; NDJSON bytes, no envelope
  // framing — the launcher pumps these lines verbatim to the runner).
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return false;
  }
  struct sockaddr_un addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  if (socket_path.size() >= sizeof(addr.sun_path)) {
    ::close(fd);
    return false;
  }
  std::memcpy(addr.sun_path, socket_path.c_str(), socket_path.size());
  if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr),
                sizeof(addr)) != 0) {
    ::close(fd);
    return false;
  }
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
  impl_->fd = fd;
  impl_->on_disconnect = std::move(on_disconnect);

  // CF-4 (design §4.1): the UNIFIED bootstrap construction — identical to
  // the register-in's (seeded identity from VELITE_PEER_SEED via the
  // vendored env path — the launcher forwards the runner's per-peer seed —
  // chrome mounted pre-seal, claims cap), composed with the SAME slot-0
  // wrapper. Two bring-up modes, ONE surface.
  impl_->bootstrap = BuildUnifiedBootstrap(
      "", MakeChromeNavHandle("legion://chrome", dispatch, cap_anchor));

  Impl* impl = impl_.get();
  impl_->disp = std::make_shared<Dispatcher>(
      MakeUnifiedSlotZero(impl_->bootstrap, std::move(dispatch), cap_anchor,
                          std::move(wire_subscribe), &impl_->mailbox),
      [impl](const std::string& frame) { impl->SendLine(frame); });
  auto bootstrap = impl_->bootstrap;
  impl_->disp->set_audit([bootstrap](std::string event, std::string trace_id) {
    bootstrap->record_audit(std::move(event), std::move(trace_id));
  });
  impl_->bootstrap->set_counterparty_dispatcher(impl_->disp.get());

  // The computed __handshake manifest — INSTEAD of the Agrippa register ask
  // (design §2.1); floor-validated by the vendored emitter.
  if (!velite::agentspaces::wire::emit_session_handshake_with_facets(
          *impl_->disp, aurelian_claimed_wire_facets(),
          aurelian_claimed_internal_facets())) {
    LOG(ERROR) << "[aurelian] conformance serve: floor-incomplete manifest";
    ::close(fd);
    impl_->fd = -1;
    return false;
  }

  impl_->connected.store(true);
  impl_->stop.store(false);
  impl_->serve_thread = std::thread([impl]() { impl->Serve(); });
  return true;
}

void ConformanceServe::Stop() {
  if (!impl_) {
    return;
  }
  impl_->stop.store(true);
  if (impl_->serve_thread.joinable()) {
    impl_->serve_thread.join();
  }
  if (impl_->fd >= 0) {
    ::close(impl_->fd);
    impl_->fd = -1;
  }
  impl_->connected.store(false);
}

bool ConformanceServe::connected() const {
  return impl_ && impl_->connected.load();
}

}  // namespace aurelian
