// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// CF-1 RED 1 (AURELIAN-CONFORMANT-FEDERATION-TDD-PLAN.md): the conformance
// serve mode. The browser, launched with --aurelian-conformance-serve=<uds>,
// must CONNECT OUT to the named UDS during PostCreateThreads (after the
// membrane seal), emit the v0.14 __handshake TELL whose wireFacets equal the
// COMPUTED claims set exactly (facet_claims.h — design §3.2: one source, two
// projections), then serve the unified slot-0 surface over raw NDJSON lines
// (the launcher is a zero-frame-aware byte pump, so the UDS carries NDJSON,
// never the envelope framing the register-in wire uses).
//
// Fixture seams (load-bearing, design CF-1):
//  - Listener-before-launch: the temp UDS is bound + listening in
//    SetUpCommandLine (the only hook that runs before the browser process
//    starts), and a fixture-owned reader thread accepts the one connection
//    and buffers inbound lines from the moment of launch — the __handshake
//    arrives during startup, asserted from the buffer, never raced.
//  - Lifetime: the fixture HOLDS the connection through the test body and
//    into teardown; the serve's exit-on-disconnect must never fire under the
//    harness (browser-side Stop() is exercised here, the launcher lane
//    exercises exit-on-disconnect).

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <deque>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "aurelian/conformance/facet_claims.h"
#include "base/command_line.h"
#include "base/synchronization/lock.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "content/public/test/browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "velite/agentspaces-wire/dispatcher.hpp"
#include "velite/agentspaces-wire/handle.hpp"
#include "velite/agentspaces-wire/value_handle.hpp"

namespace aurelian {
namespace {

using velite::agentspaces::Handle;
using velite::agentspaces::StateKind;
using velite::agentspaces::Value;
using velite::agentspaces::ValueHandle;
using velite::agentspaces::wire::Dispatcher;

// The driver side (what the launcher/runner is): slot-0 bootstrap. sessions.md
// §2a retired the proactive __handshake TELL — the responder is silent until it
// receives a propose-session, then replies with an ack-session. This driver
// records whether ANY legacy __handshake arrives (it MUST NOT) and otherwise
// drives the connector leg.
class DriverBootstrap : public Handle {
 public:
  static std::shared_ptr<DriverBootstrap> make() {
    return std::shared_ptr<DriverBootstrap>(new DriverBootstrap());
  }
  StateKind state_kind() const override { return StateKind::ResolvedValue; }
  const Value& resolved_value() const override { return self_; }
  std::shared_ptr<Handle> resolved_handle() const override { return nullptr; }
  std::string_view broken_reason() const override { return ""; }
  std::shared_ptr<Handle> ask_impl(std::string_view,
                                   const Value&) override {
    return ValueHandle::make_broken("legion://errors/UnknownMessage");
  }
  void tell(std::string_view msg, const Value&) override {
    if (msg == "__handshake") {
      saw_handshake = true;  // §2a: the legacy frame must NEVER fire.
    }
  }
  bool saw_handshake = false;

 private:
  Value self_{std::string("driver")};
};

}  // namespace

class AurelianConformanceServeBrowserTest : public InProcessBrowserTest {
 protected:
  // Listener-before-launch: bind + listen + start the buffering reader
  // BEFORE the switch is appended, so the serve's connect-out during
  // PostCreateThreads always finds a listener and the startup __handshake
  // is buffered, never raced.
  void SetUpCommandLine(base::CommandLine* command_line) override {
    sock_path_ = std::string("/tmp/aurelian-cf1-") +
                 std::to_string(::getpid()) + ".sock";
    ::unlink(sock_path_.c_str());

    listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_GE(listen_fd_, 0);
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    ASSERT_LT(sock_path_.size(), sizeof(addr.sun_path));
    std::memcpy(addr.sun_path, sock_path_.c_str(), sock_path_.size());
    ASSERT_EQ(::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr),
                     sizeof(addr)),
              0);
    ASSERT_EQ(::listen(listen_fd_, 1), 0);

    reader_ = std::thread([this]() { ReaderMain(); });

    command_line->AppendSwitchASCII("aurelian-conformance-serve", sock_path_);
  }

  void TearDownInProcessBrowserTestFixture() override {
    stop_.store(true);
    if (reader_.joinable()) {
      reader_.join();
    }
    if (conn_fd_ >= 0) {
      ::close(conn_fd_);
    }
    if (listen_fd_ >= 0) {
      ::close(listen_fd_);
    }
    ::unlink(sock_path_.c_str());
  }

  // The reader thread: accept the ONE connection, then buffer raw bytes,
  // splitting NDJSON lines into the guarded deque.
  void ReaderMain() {
    struct pollfd pfd;
    pfd.fd = listen_fd_;
    pfd.events = POLLIN;
    while (!stop_.load()) {
      if (::poll(&pfd, 1, 50) > 0 && (pfd.revents & POLLIN)) {
        conn_fd_ = ::accept(listen_fd_, nullptr, nullptr);
        break;
      }
    }
    if (conn_fd_ < 0) {
      return;
    }
    accepted_.store(true);
    std::string acc;
    char buf[65536];
    struct pollfd cfd;
    cfd.fd = conn_fd_;
    cfd.events = POLLIN;
    while (!stop_.load()) {
      if (::poll(&cfd, 1, 50) > 0 && (cfd.revents & (POLLIN | POLLHUP))) {
        ssize_t n = ::read(conn_fd_, buf, sizeof(buf));
        if (n <= 0) {
          break;  // peer closed — the fixture holds ITS side open regardless
        }
        acc.append(buf, static_cast<size_t>(n));
        for (;;) {
          size_t nl = acc.find('\n');
          if (nl == std::string::npos) {
            break;
          }
          std::string line = acc.substr(0, nl);
          acc.erase(0, nl + 1);
          if (!line.empty()) {
            base::AutoLock hold(lines_lock_);
            lines_.push_back(std::move(line));
          }
        }
      }
    }
  }

  bool PopLine(std::string* out) {
    base::AutoLock hold(lines_lock_);
    if (lines_.empty()) {
      return false;
    }
    *out = std::move(lines_.front());
    lines_.pop_front();
    return true;
  }

  bool WriteLine(const std::string& frame) {
    std::string line = frame;
    line.push_back('\n');
    const char* p = line.data();
    size_t left = line.size();
    while (left > 0) {
      ssize_t n = ::send(conn_fd_, p, left, 0);
      if (n <= 0) {
        return false;
      }
      p += n;
      left -= static_cast<size_t>(n);
    }
    return true;
  }

  std::string sock_path_;
  int listen_fd_ = -1;
  int conn_fd_ = -1;
  std::atomic<bool> stop_{false};
  std::atomic<bool> accepted_{false};
  std::thread reader_;
  base::Lock lines_lock_;
  std::deque<std::string> lines_;
};

IN_PROC_BROWSER_TEST_F(AurelianConformanceServeBrowserTest,
                       HandshakeThenPingOverUds) {
  // The serve connected out during PostCreateThreads — the accept must
  // already have happened (or happen near-instantly).
  for (int i = 0; i < 4000 && !accepted_.load(); ++i) {
    base::PlatformThread::Sleep(base::Milliseconds(1));
  }
  ASSERT_TRUE(accepted_.load())
      << "the browser must CONNECT OUT to --aurelian-conformance-serve=<uds> "
         "during PostCreateThreads";

  // Driver-side dispatcher over the raw NDJSON wire.
  auto driver_bs = DriverBootstrap::make();
  Dispatcher driver(driver_bs,
                    [this](const std::string& f) { WriteLine(f); });
  std::string ack_frame;
  auto pump = [&]() {
    std::string line;
    while (PopLine(&line)) {
      if (line.find("\"op\":\"ack-session\"") != std::string::npos) {
        ack_frame = line;
      }
      driver.on_inbound(line);
    }
    driver.pump_pending_answers();
  };

  // 1. sessions.md §2a: the peer is a RESPONDER — silent until it receives a
  // propose-session, then it replies with an ack-session carrying ITS facet
  // manifest (supports ≡ aurelian_claimed_wire_facets(), peer ==
  // legion://peers/aurelian), and NEVER a proactive __handshake TELL (the legacy
  // frame is retired). Drive the connector's propose; capture the ack.
  driver.emit_propose_session();
  for (int i = 0; i < 4000 && ack_frame.empty(); ++i) {
    pump();
    base::PlatformThread::Sleep(base::Milliseconds(1));
  }
  ASSERT_FALSE(ack_frame.empty())
      << "the §2a responder must reply to propose-session with an ack-session "
         "(sessions.md §2a.3 / [SESSION-MANIFEST-IN-HANDSHAKE-ENVELOPE])";
  ASSERT_FALSE(driver_bs->saw_handshake)
      << "the legacy proactive __handshake TELL must be retired (sessions.md §2a)";
  EXPECT_NE(ack_frame.find("\"peer\":\"legion://peers/aurelian\""),
            std::string::npos)
      << "the ack manifest must advertise legion://peers/aurelian, got: "
      << ack_frame;
  for (const std::string& facet : aurelian_claimed_wire_facets()) {
    EXPECT_NE(ack_frame.find(facet), std::string::npos)
        << "the ack manifest 'supports' must carry claimed wire facet " << facet;
  }

  // 2. ping -> resolved "pong" over the wire (the vendored corpus verb on
  // the unified slot-0 surface).
  uint32_t ping_slot = driver.emit_ask(0, "ping", Value());
  Dispatcher::AnswerOutcome ping_ans;
  for (int i = 0; i < 4000; ++i) {
    pump();
    ping_ans = driver.answer_outcome(ping_slot);
    if (ping_ans.settled) {
      break;
    }
    base::PlatformThread::Sleep(base::Milliseconds(1));
  }
  ASSERT_TRUE(ping_ans.settled) << "ping must settle over the wire";
  EXPECT_TRUE(ping_ans.ok) << ping_ans.reason;
  EXPECT_TRUE(ping_ans.value.is_string() &&
              ping_ans.value.as_string() == "pong")
      << "ping must resolve \"pong\"";

  // 3. getResource of an UNCLAIMED facility declines TYPED (the §3.2 claims
  // cap, pinned RED-first): datastore is queued CF-EXT, compiled-but-capped.
  uint32_t gr_slot = driver.emit_ask(
      0, "getResource",
      Value::make_object(
          {{"uri", Value(std::string("legion://datastore"))}}));
  Dispatcher::AnswerOutcome gr_ans;
  for (int i = 0; i < 4000; ++i) {
    pump();
    gr_ans = driver.answer_outcome(gr_slot);
    if (gr_ans.settled) {
      break;
    }
    base::PlatformThread::Sleep(base::Milliseconds(1));
  }
  ASSERT_TRUE(gr_ans.settled);
  EXPECT_FALSE(gr_ans.ok)
      << "an unclaimed facility must NOT be acquirable "
         "([LAYERS-NO-CONFORMANCE-BY-OMISSION])";
  // TEST-CHANGE (AU-ERRORS-WIRE-PAIR): this asserted the typed URI inside
  // `reason`, which errors.md 3.1 forbids — [ERRORS-WIRE-REASON-IS-KEBAB-WIRECODE]
  // puts the kebab wireCode in `reason` and [ERRORS-WIRE-CODE-CARRIES-TYPED-URI]
  // puts the URI in `code`. It also named the wrong namespace: the error is a
  // Level-2 extension, `legion://errors/velite-cpp/FacetNotClaimed`, and an
  // extension code KEEPS its namespace
  // ([ERRORS-EXTENSION-CODE-SUBSUMES-TO-LEVEL1]). The discriminator naming WHICH
  // facility was declined is diagnostic detail and rides `details`, never the
  // reason.
  EXPECT_EQ(gr_ans.reason, "facet-not-claimed")
      << "the decline must be the kebab wireCode, got: " << gr_ans.reason;
  EXPECT_EQ(gr_ans.code, "legion://errors/velite-cpp/FacetNotClaimed")
      << "the typed URI rides `code`, got: " << gr_ans.code;
}

}  // namespace aurelian
