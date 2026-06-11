// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// CF-5 RED (AURELIAN-CONFORMANT-FEDERATION-TDD-PLAN): wire eventing — a CDP
// event must CROSS THE WIRE as the vendored `legion-subscription-event`
// TELL ({sub_id, msg: legion-notify, event}, byte-compatible with
// SubSinkHandle::tell — design §5.1: consume, never invent). The test plays
// the hub over the register-in UDS (the boot_register fixture shape):
// subscribe to a chrome event node via `legion-subscribe-remote`, open a
// real tab in-process, and expect the Target.targetCreated event frame on
// the wire. RED today: the verb delegates to the vendored KG (which holds
// no chrome event nodes) — the subscribe may ack, but no TELL ever arrives.

#include <unistd.h>

#include <map>
#include <string>
#include <vector>

#include "aurelian/federation/uds_register.h"
#include "aurelian/federation/wire_event_mailbox.h"
#include "base/run_loop.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/platform_thread.h"
#include "base/threading/thread_restrictions.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_tabstrip.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/test/browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"
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

// The hub side: records the register ask AND every legion-subscription-event
// TELL the browser emits at slot 0.
class EventHubBootstrap : public Handle {
 public:
  static std::shared_ptr<EventHubBootstrap> make() {
    return std::shared_ptr<EventHubBootstrap>(new EventHubBootstrap());
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
  void tell(std::string_view msg, const Value& data) override {
    if (msg == "legion-subscription-event") {
      events.push_back(data);
    }
  }
  std::string registered;
  std::vector<Value> events;

 private:
  Value self_{std::string("event-hub")};
};

}  // namespace

class AurelianWireEventBrowserTest : public InProcessBrowserTest {
 protected:
  void SetUpInProcessBrowserTestFixture() override {
    base::ScopedAllowBlockingForTesting allow_blocking;
    sock_ = std::string("/tmp/aurelian-cf5-") + std::to_string(::getpid()) +
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

IN_PROC_BROWSER_TEST_F(AurelianWireEventBrowserTest,
                       CdpEventCrossesWireAsSubscriptionEventTell) {
  base::ScopedAllowBlockingForTesting allow_blocking;

  // Accept the boot register-in dial.
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
  auto hub_bs = EventHubBootstrap::make();
  Dispatcher hub(hub_bs, [raw](const std::string& f) {
    raw->send(reinterpret_cast<const uint8_t*>(f.data()), f.size());
  });
  uint8_t buf[65536];
  auto pump_hub = [&]() {
    for (;;) {
      size_t n = 0;
      if (server.recv(buf, sizeof(buf), &n) == velite::ChannelError::OK &&
          n > 0) {
        hub.on_inbound(std::string(reinterpret_cast<const char*>(buf), n));
      } else {
        break;
      }
    }
    hub.pump_pending_answers();
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

  // Subscribe to the browser-target event node over the wire (the vendored
  // verb pair — no invented framing, design §5.1).
  uint32_t sub_slot = hub.emit_ask(
      0, "legion-subscribe-remote",
      Value::make_object(
          {{"uri", Value(std::string(
                "legion://chrome/cdp/Target/events/targetCreated"))},
           {"sub_id", Value(std::string("cf5-probe"))}}));
  Dispatcher::AnswerOutcome sub_ans;
  for (int i = 0; i < 1600; ++i) {
    pump_hub();
    sub_ans = hub.answer_outcome(sub_slot);
    if (sub_ans.settled) {
      break;
    }
    spin();
  }
  ASSERT_TRUE(sub_ans.settled) << "the wire subscribe must settle";
  EXPECT_TRUE(sub_ans.ok)
      << "the subscribe must ack against the chrome event-node surface, "
         "got: " << sub_ans.reason;

  // Drive the event: open a REAL tab in-process — the browser target's
  // Target.targetCreated fires.
  chrome::AddTabAt(browser(), GURL("about:blank"), -1, /*foreground=*/true);

  // Expect the vendored legion-subscription-event TELL on the wire:
  // {sub_id: "cf5-probe", msg: "legion-notify", event: {...}}.
  for (int i = 0; i < 1600 && hub_bs->events.empty(); ++i) {
    pump_hub();
    spin();
  }
  ASSERT_FALSE(hub_bs->events.empty())
      << "a CDP event must cross the wire as legion-subscription-event "
         "(design §5.2 — the mailbox/WireSinkHandle bridge)";
  const Value& ev = hub_bs->events.front();
  const Value* sid = ev.object_get("sub_id");
  ASSERT_TRUE(sid && sid->is_string());
  EXPECT_EQ(sid->as_string(), "cf5-probe");
  const Value* m = ev.object_get("msg");
  ASSERT_TRUE(m && m->is_string());
  EXPECT_EQ(m->as_string(), "legion-notify")
      << "the SubSinkHandle frame shape, byte-compatible (design §5.1)";
  EXPECT_NE(ev.object_get("event"), nullptr)
      << "the event payload must ride the `event` field";
}

// CF-5 — the overflow bound (design §5.2): past the per-subscription cap
// the subscription terminates with the typed
// legion://errors/SubscriptionOverflow cancelled notify, and NO further
// frames flow — never silent loss. Cap pinned to ZERO via the fixture-set
// default (the boot path owns its mailbox), so the FIRST event overflows
// deterministically — the degenerate flood; the bound's machinery (terminal
// notify + producer-side cancel + post-overflow drop) is what the pin
// proves. NOTE (worklog): the bound was built in this same slice before
// this pin first ran — recorded as a PIN (the ACM-2 UnknownCommandTyped
// precedent), not a RED.
class AurelianWireEventOverflowBrowserTest
    : public AurelianWireEventBrowserTest {
 protected:
  void SetUpInProcessBrowserTestFixture() override {
    WireEventMailbox::SetDefaultPerSubscriptionCapForTesting(0);
    AurelianWireEventBrowserTest::SetUpInProcessBrowserTestFixture();
  }
  void TearDownInProcessBrowserTestFixture() override {
    WireEventMailbox::SetDefaultPerSubscriptionCapForTesting(SIZE_MAX);
    AurelianWireEventBrowserTest::TearDownInProcessBrowserTestFixture();
  }
};

IN_PROC_BROWSER_TEST_F(AurelianWireEventOverflowBrowserTest,
                       MailboxOverflowTerminatesTyped) {
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
  auto hub_bs = EventHubBootstrap::make();
  Dispatcher hub(hub_bs, [raw](const std::string& f) {
    raw->send(reinterpret_cast<const uint8_t*>(f.data()), f.size());
  });
  uint8_t buf[65536];
  auto pump_hub = [&]() {
    for (;;) {
      size_t n = 0;
      if (server.recv(buf, sizeof(buf), &n) == velite::ChannelError::OK &&
          n > 0) {
        hub.on_inbound(std::string(reinterpret_cast<const char*>(buf), n));
      } else {
        break;
      }
    }
    hub.pump_pending_answers();
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

  uint32_t sub_slot = hub.emit_ask(
      0, "legion-subscribe-remote",
      Value::make_object(
          {{"uri", Value(std::string(
                "legion://chrome/cdp/Target/events/targetCreated"))},
           {"sub_id", Value(std::string("cf5-overflow"))}}));
  Dispatcher::AnswerOutcome sub_ans;
  for (int i = 0; i < 1600; ++i) {
    pump_hub();
    sub_ans = hub.answer_outcome(sub_slot);
    if (sub_ans.settled) {
      break;
    }
    spin();
  }
  ASSERT_TRUE(sub_ans.settled && sub_ans.ok) << sub_ans.reason;

  // The first event overflows the zero cap: expect EXACTLY the terminal
  // cancelled notify, typed.
  chrome::AddTabAt(browser(), GURL("about:blank"), -1, /*foreground=*/true);
  for (int i = 0; i < 1600 && hub_bs->events.empty(); ++i) {
    pump_hub();
    spin();
  }
  ASSERT_FALSE(hub_bs->events.empty())
      << "overflow must surface the typed terminal notify, never silence";
  {
    const Value& ev = hub_bs->events.front();
    const Value* sid = ev.object_get("sub_id");
    ASSERT_TRUE(sid && sid->is_string());
    EXPECT_EQ(sid->as_string(), "cf5-overflow");
    const Value* body = ev.object_get("event");
    ASSERT_TRUE(body && body->is_object());
    const Value* state = body->object_get("state");
    ASSERT_TRUE(state && state->is_string());
    EXPECT_EQ(state->as_string(), "cancelled");
    const Value* reason = body->object_get("reason");
    ASSERT_TRUE(reason && reason->is_string());
    EXPECT_EQ(reason->as_string(), "legion://errors/SubscriptionOverflow")
        << "the bound terminates TYPED (design §5.2)";
  }

  // The subscription is dead: further events produce NO frames (the
  // producer side is cancelled, the mailbox drops).
  const size_t frames_after_terminal = hub_bs->events.size();
  chrome::AddTabAt(browser(), GURL("about:blank"), -1, /*foreground=*/true);
  for (int i = 0; i < 400; ++i) {
    pump_hub();
    spin();
  }
  EXPECT_EQ(hub_bs->events.size(), frames_after_terminal)
      << "no frames may follow the terminal overflow notify";
}

}  // namespace aurelian
