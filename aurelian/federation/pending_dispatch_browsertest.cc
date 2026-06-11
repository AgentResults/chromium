// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// CF-6 RED (AURELIAN-CONFORMANT-FEDERATION-TDD-PLAN): F8b — deferred
// pending answers lift the one-in-flight-per-connection limit. Over the
// register-in wire (test-as-hub): ask A is a chrome CDP dispatch whose
// completion needs UI turns the TEST controls (the ACM-2 two-in-flight
// determinism lever — a posted dispatch cannot complete until this thread
// spins the UI loop); ask B is `ping` (settles serve-side, no UI hop). The
// pin: B's __resolve must arrive WHILE A is still pending — N>1 in-flight
// is structural once the serve thread never blocks between frames.
//
// RED today: the serve thread is blocked inside the HS-1 blocking bridge
// for A (one-in-flight), so B is not even read until A settles — the
// observed settle order is A-then-B, failing the B-before-A pin.

#include <unistd.h>

#include <string>
#include <vector>

#include "base/run_loop.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/platform_thread.h"
#include "base/threading/thread_restrictions.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "content/public/test/browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "velite/agentspaces-wire/dispatcher.hpp"
#include "velite/agentspaces-wire/handle.hpp"
#include "velite/agentspaces-wire/value_handle.hpp"
#include "velite/channel.hpp"
#include "velite/uds_channel.hpp"
#include "velite/uds_listener.hpp"

namespace aurelian {
namespace {

using velite::agentspaces::Handle;
using velite::agentspaces::StateKind;
using velite::agentspaces::Value;
using velite::agentspaces::ValueHandle;
using velite::agentspaces::wire::Dispatcher;

class PendingHubBootstrap : public Handle {
 public:
  static std::shared_ptr<PendingHubBootstrap> make() {
    return std::shared_ptr<PendingHubBootstrap>(new PendingHubBootstrap());
  }
  StateKind state_kind() const override { return StateKind::ResolvedValue; }
  const Value& resolved_value() const override { return self_; }
  std::shared_ptr<Handle> resolved_handle() const override { return nullptr; }
  std::string_view broken_reason() const override { return ""; }
  std::shared_ptr<Handle> ask_impl(std::string_view msg,
                                   const Value& spec) override {
    if (msg == "update") {
      const Value* name = spec.is_object() ? spec.object_get("name") : nullptr;
      if (name && name->is_string()) {
        registered = name->as_string();
      }
      return ValueHandle::make(Value(std::string("ok")));
    }
    return ValueHandle::make_broken("legion://errors/UnknownMessage");
  }
  void tell(std::string_view, const Value&) override {}
  std::string registered;

 private:
  Value self_{std::string("pending-hub")};
};

}  // namespace

class AurelianPendingDispatchBrowserTest : public InProcessBrowserTest {
 protected:
  void SetUpInProcessBrowserTestFixture() override {
    base::ScopedAllowBlockingForTesting allow_blocking;
    sock_ = std::string("/tmp/aurelian-cf6-") + std::to_string(::getpid()) +
            ".sock";
    ::unlink(sock_.c_str());
    ASSERT_TRUE(listener_.listen(sock_.c_str())) << "hub stand-in bind";
    ::setenv("AGRIPPA_UDS_PATH", sock_.c_str(), /*overwrite=*/1);
  }

  void TearDownInProcessBrowserTestFixture() override {
    ::unlink(sock_.c_str());
  }

  std::string sock_;
  velite::UdsListener listener_;
};

IN_PROC_BROWSER_TEST_F(AurelianPendingDispatchBrowserTest,
                       SecondAskSettlesWhileFirstInFlight) {
  base::ScopedAllowBlockingForTesting allow_blocking;

  velite::UdsChannel server;
  velite::UdsListener::AcceptOutcome out =
      velite::UdsListener::AcceptOutcome::NoneReady;
  for (int i = 0;
       i < 4000 && out != velite::UdsListener::AcceptOutcome::Accepted; ++i) {
    out = listener_.accept(server, nullptr);
    if (out == velite::UdsListener::AcceptOutcome::NoneReady) {
      base::PlatformThread::Sleep(base::Milliseconds(1));
    }
  }
  ASSERT_EQ(out, velite::UdsListener::AcceptOutcome::Accepted);

  velite::UdsChannel* raw = &server;
  auto hub_bs = PendingHubBootstrap::make();
  Dispatcher hub(hub_bs, [raw](const std::string& f) {
    raw->send(reinterpret_cast<const uint8_t*>(f.data()), f.size());
  });
  uint8_t buf[65536];
  // Settle-order ledger: which of {A, B} settled, in arrival order.
  std::vector<char> settle_order;
  uint32_t slot_a = 0;
  uint32_t slot_b = 0;
  // The ledger notes settlement after EVERY inbound frame — never once per
  // pump pass (both resolves can arrive within one pass; the WIRE order is
  // what the pin asserts).
  auto note = [&]() {
    if (slot_a && hub.answer_outcome(slot_a).settled &&
        std::find(settle_order.begin(), settle_order.end(), 'A') ==
            settle_order.end()) {
      settle_order.push_back('A');
    }
    if (slot_b && hub.answer_outcome(slot_b).settled &&
        std::find(settle_order.begin(), settle_order.end(), 'B') ==
            settle_order.end()) {
      settle_order.push_back('B');
    }
  };
  auto pump_hub = [&]() {
    for (;;) {
      size_t n = 0;
      if (server.recv(buf, sizeof(buf), &n) == velite::ChannelError::OK &&
          n > 0) {
        hub.on_inbound(std::string(reinterpret_cast<const char*>(buf), n));
        note();  // frame-granular: the wire order, not the pass order
      } else {
        break;
      }
    }
    hub.pump_pending_answers();
    note();
  };
  auto spin = [&]() {
    base::RunLoop loop;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, loop.QuitClosure(), base::Milliseconds(5));
    loop.Run();
  };
  for (int i = 0; i < 800 && hub_bs->registered.empty(); ++i) {
    pump_hub();
    spin();
  }
  ASSERT_EQ(hub_bs->registered, "chrome");

  // Ask A: a REAL chrome CDP dispatch (posted to the UI thread; completes
  // only across the UI turns this test spins). Ask B: ping — pure
  // serve-thread, no UI hop. Emitted back-to-back on ONE connection.
  slot_a = hub.emit_ask(
      0, "dispatchAt",
      Value::make_object(
          {{"uri",
            Value(std::string("legion://chrome/cdp/Browser/getVersion"))},
           {"verb", Value(std::string("invoke"))}}));
  slot_b = hub.emit_ask(0, "ping", Value());

  // Drive until both settle (the spin both pumps the wire AND lets the UI
  // task complete A).
  for (int i = 0; i < 3200 && settle_order.size() < 2; ++i) {
    pump_hub();
    spin();
  }
  ASSERT_EQ(settle_order.size(), 2u)
      << "both asks must settle (A=" << hub.answer_outcome(slot_a).settled
      << " B=" << hub.answer_outcome(slot_b).settled << ")";

  // THE PIN: B settled BEFORE A — the serve thread answered ping while the
  // chrome dispatch was still in flight (one-in-flight LIFTED).
  EXPECT_EQ(settle_order[0], 'B')
      << "B must settle while A is in flight (F8b — the serve thread never "
         "blocks between frames); observed order A-then-B is the "
         "one-in-flight limit";

  // And A settles CORRECTLY (the real getVersion reply, ok).
  Dispatcher::AnswerOutcome a = hub.answer_outcome(slot_a);
  EXPECT_TRUE(a.ok) << a.reason;
  ASSERT_TRUE(a.value.is_string());
  EXPECT_NE(a.value.as_string().find("product"), std::string::npos)
      << "A must settle with the real CDP reply, got: " << a.value.as_string();

  // B was the canonical pong.
  Dispatcher::AnswerOutcome b = hub.answer_outcome(slot_b);
  EXPECT_TRUE(b.ok && b.value.is_string() && b.value.as_string() == "pong");
}

}  // namespace aurelian
