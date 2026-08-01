// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// CF-8 RED (AURELIAN-CONFORMANT-FEDERATION-TDD-PLAN): nav-surface cap
// binding — ONE authorization model (design §7). A scoped cap that admits
// dispatchAt within its subtree must ALSO gate NAVIGATION: getResource on a
// chrome URI accepts an optional `cap`, the returned handle is BORN BOUND to
// the verified chain, bound leaf asks re-run the gate per ask (`now`
// re-evaluated), child minting composes AND-only (parent ∧ presented), and
// the wire subscribe passes the same gate BEFORE any registration. RED
// today: getResource ignores the cap entirely (uds_register.cc — no gate on
// either navigation path), so an out-of-subtree handle resolves on
// connection-level authority and its leaf asks SUCCEED; the subscribe
// registers and events flow. Real Ed25519 chains, real boot UDS, the REAL
// sealed root — no mocks.

#include <unistd.h>

#include <map>
#include <string>
#include <vector>

#include "aurelian/capability/cap_chain.h"
#include "aurelian/capability/cap_wire.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/platform_thread.h"
#include "base/threading/thread_restrictions.h"
#include "base/time/time.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_tabstrip.h"
#include "chrome/test/base/in_process_browser_test.h"
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
// TELL (the wire_event_browsertest recorder shape — the subscribe legs
// assert on OBSERVED frames, never on returned-but-not-asserted acks).
class NavCapHubBootstrap : public Handle {
 public:
  static std::shared_ptr<NavCapHubBootstrap> make() {
    return std::shared_ptr<NavCapHubBootstrap>(new NavCapHubBootstrap());
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
  Value self_{std::string("nav-cap-hub")};
};

}  // namespace

// The boot fixture with the operator anchor published (the ACM-8
// AurelianBootCapGateBrowserTest shape): AGRIPPA_UDS_PATH + the anchor env
// are read in PostCreateThreads, before any test body.
class AurelianNavCapBrowserTest : public InProcessBrowserTest {
 protected:
  void SetUpInProcessBrowserTestFixture() override {
    base::ScopedAllowBlockingForTesting allow_blocking;
    sock_ = std::string("/tmp/aurelian-cf8-") + std::to_string(::getpid()) +
            ".sock";
    ::unlink(sock_.c_str());
    ASSERT_TRUE(listener_.listen(sock_.c_str())) << "hub stand-in bind";
    ::setenv("AGRIPPA_UDS_PATH", sock_.c_str(), /*overwrite=*/1);
    anchor_priv_.fill(0xC8);
    PubKey pub = PubFromPriv(anchor_priv_);
    ::setenv("AURELIAN_CAP_ANCHOR", base::HexEncode(pub).c_str(),
             /*overwrite=*/1);
  }

  void TearDownInProcessBrowserTestFixture() override {
    ::unsetenv("AURELIAN_CAP_ANCHOR");
    ::unlink(sock_.c_str());
  }

  // A single-link operator->controller chain scoped to `pattern`,
  // optionally expiring (0 = no expiry) — the ACM-8 att-1 shape.
  std::string ScopedCap(const std::string& pattern, int64_t expires = 0) {
    PrivKey controller{};
    controller.fill(0xC9);
    CapLink link;
    link.cap_id = "root";
    link.parent_cap_id = "";
    link.predicate = "pattern=" + pattern;
    link.expires = expires;
    link.issuer_pub = PubFromPriv(anchor_priv_);
    link.subject_pub = PubFromPriv(controller);
    EXPECT_TRUE(SignLink(&link, anchor_priv_));
    return SerializeChain({link});
  }

  // Accept the boot dial + stand the hub Dispatcher up; returns false on
  // accept timeout.
  bool StartHub() {
    velite::UdsListener::AcceptOutcome out =
        velite::UdsListener::AcceptOutcome::NoneReady;
    for (int i = 0;
         i < 4000 && out != velite::UdsListener::AcceptOutcome::Accepted;
         ++i) {
      out = listener_.accept(server_, nullptr);
      if (out == velite::UdsListener::AcceptOutcome::NoneReady) {
        base::PlatformThread::Sleep(base::Milliseconds(1));
      }
    }
    if (out != velite::UdsListener::AcceptOutcome::Accepted) {
      return false;
    }
    hub_bs_ = NavCapHubBootstrap::make();
    velite::UdsChannel* raw = &server_;
    hub_ = std::make_unique<Dispatcher>(hub_bs_, [raw](const std::string& f) {
      raw->send(reinterpret_cast<const uint8_t*>(f.data()), f.size());
    });
    for (int i = 0; i < 800 && hub_bs_->registered.empty(); ++i) {
      PumpHub();
      Spin();
    }
    return hub_bs_->registered == "chrome";
  }

  void PumpHub() {
    for (;;) {
      size_t n = 0;
      if (server_.recv(buf_, sizeof(buf_), &n) == velite::ChannelError::OK &&
          n > 0) {
        hub_->on_inbound(std::string(reinterpret_cast<const char*>(buf_), n));
      } else {
        break;
      }
    }
    hub_->pump_pending_answers();
  }

  void Spin() {
    base::RunLoop loop;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, loop.QuitClosure(), base::Milliseconds(5));
    loop.Run();
  }

  Dispatcher::AnswerOutcome Settle(uint32_t slot) {
    Dispatcher::AnswerOutcome ans;
    for (int i = 0; i < 1600; ++i) {
      PumpHub();
      ans = hub_->answer_outcome(slot);
      if (ans.settled) {
        break;
      }
      Spin();
    }
    return ans;
  }

  // getResource{uri, cap?} against `target` (slot 0 or a navigable child).
  Dispatcher::AnswerOutcome GetResource(uint32_t target,
                                        const std::string& uri,
                                        const std::string& cap = {}) {
    std::map<std::string, Value> spec{{"uri", Value(uri)}};
    if (!cap.empty()) {
      spec.emplace("cap", Value(cap));
    }
    return Settle(hub_->emit_ask(target, "getResource",
                                 Value::make_object(std::move(spec))));
  }

  Dispatcher::AnswerOutcome LeafAsk(uint32_t slot, const std::string& verb) {
    return Settle(hub_->emit_ask(slot, verb, Value()));
  }

  std::string sock_;
  velite::UdsListener listener_;
  velite::UdsChannel server_;
  std::shared_ptr<NavCapHubBootstrap> hub_bs_;
  std::unique_ptr<Dispatcher> hub_;
  uint8_t buf_[65536];
  PrivKey anchor_priv_{};
};

// (1) The plan RED: a scoped cap presented at getResource BINDS the acquired
// handle. In-subtree: the handle resolves and its leaf ask reaches the REAL
// node. Out-of-subtree under the SAME cap: the acquisition refuses typed
// (cap-refused:pattern-mismatch) — today it resolves on connection-level
// authority and the leaf ask SUCCEEDS (that success is the banked RED).
IN_PROC_BROWSER_TEST_F(AurelianNavCapBrowserTest,
                       ScopedCapBindsAcquiredHandle) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  ASSERT_TRUE(StartHub());

  const std::string cap = ScopedCap("legion://chrome/system/*");

  // In-subtree acquisition admitted; the bound handle's leaf ask reaches the
  // REAL system node (base-node admission: system tested as "system/").
  Dispatcher::AnswerOutcome in_tree =
      GetResource(0, "legion://chrome/system", cap);
  ASSERT_TRUE(in_tree.settled && in_tree.ok)
      << "in-subtree getResource under its own cap must resolve: "
      << in_tree.reason;
  ASSERT_TRUE(in_tree.value.is_slot_ref());
  Dispatcher::AnswerOutcome info =
      LeafAsk(in_tree.value.as_slot_ref().slot, "info");
  EXPECT_TRUE(info.settled && info.ok) << info.reason;
  // TEST-CHANGE (AU-WIRE-KIND): system/info is a STRUCTURED answer now, not
  // JSON-in-a-string. is_string() compiles fine against an object, so the build
  // could not catch this — the reason the migration missed it.
  ASSERT_TRUE(info.value.is_object())
      << "the bound handle's in-subtree leaf ask must reach the real node";
  EXPECT_NE(info.value.object_get("browserPid"), nullptr)
      << "the real node's info must carry browserPid";

  // Out-of-subtree acquisition under the SAME cap refuses typed, and no
  // handle exists to ask.
  Dispatcher::AnswerOutcome out_tree =
      GetResource(0, "legion://chrome/tabs", cap);
  ASSERT_TRUE(out_tree.settled);
  if (out_tree.ok && out_tree.value.is_slot_ref()) {
    // The RED path: the cap was ignored; prove the leaked authority is real.
    Dispatcher::AnswerOutcome count =
        LeafAsk(out_tree.value.as_slot_ref().slot, "count");
    ADD_FAILURE() << "out-of-subtree getResource RESOLVED under a "
                     "system/*-scoped cap (connection-level authority leak); "
                     "leaf count="
                  << (count.value.is_string() ? count.value.as_string()
                                              : count.reason);
  }
  EXPECT_FALSE(out_tree.ok);
  EXPECT_NE(out_tree.reason.find("cap-refused"), std::string::npos)
      << out_tree.reason;
  EXPECT_NE(out_tree.reason.find("pattern-mismatch"), std::string::npos)
      << out_tree.reason;
}

// (2) The plan RED: child minting from a BOUND parent composes AND-only —
// parent ∧ presented. A WIDER presented cap cannot escape the parent's
// subtree (attenuation only narrows, constraints.md §5); a capless child
// inherits the parent chain and stays inside it.
IN_PROC_BROWSER_TEST_F(AurelianNavCapBrowserTest,
                       DeepChainInheritsIntersection) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  ASSERT_TRUE(StartHub());

  Dispatcher::AnswerOutcome parent = GetResource(
      0, "legion://chrome/system", ScopedCap("legion://chrome/system/*"));
  ASSERT_TRUE(parent.settled && parent.ok) << parent.reason;
  ASSERT_TRUE(parent.value.is_slot_ref());
  const uint32_t parent_slot = parent.value.as_slot_ref().slot;

  // Out-of-parent-subtree child with a WIDER presented cap: parent ∧
  // presented refuses (parent's system/* does not admit tabs).
  Dispatcher::AnswerOutcome child_out = GetResource(
      parent_slot, "legion://chrome/tabs", ScopedCap("legion://chrome/*"));
  ASSERT_TRUE(child_out.settled);
  if (child_out.ok && child_out.value.is_slot_ref()) {
    Dispatcher::AnswerOutcome count =
        LeafAsk(child_out.value.as_slot_ref().slot, "count");
    ADD_FAILURE() << "a WIDER presented cap escaped the bound parent's "
                     "subtree (no AND-composition); leaf count="
                  << (count.value.is_string() ? count.value.as_string()
                                              : count.reason);
  }
  EXPECT_FALSE(child_out.ok);
  EXPECT_NE(child_out.reason.find("cap-refused"), std::string::npos)
      << child_out.reason;
  EXPECT_NE(child_out.reason.find("pattern-mismatch"), std::string::npos)
      << child_out.reason;

  // Capless in-subtree child INHERITS the parent chain and works.
  Dispatcher::AnswerOutcome child_in =
      GetResource(parent_slot, "legion://chrome/system");
  ASSERT_TRUE(child_in.settled && child_in.ok)
      << "an in-subtree capless child must inherit and resolve: "
      << child_in.reason;
  ASSERT_TRUE(child_in.value.is_slot_ref());
  Dispatcher::AnswerOutcome info =
      LeafAsk(child_in.value.as_slot_ref().slot, "info");
  EXPECT_TRUE(info.settled && info.ok) << info.reason;
  // TEST-CHANGE (AU-WIRE-KIND): structured, not JSON-in-a-string (see above).
  ASSERT_TRUE(info.value.is_object());
  EXPECT_NE(info.value.object_get("browserPid"), nullptr);
}

// (3) The plan RED: the wire subscribe is an ask and passes the SAME gate
// BEFORE any registration (subscribe.md §5 at-subscribe-time intersection).
// An out-of-cap event-node URI refuses typed AND no events ever flow — today
// the cap is ignored, the subscription registers, and the event arrives.
IN_PROC_BROWSER_TEST_F(AurelianNavCapBrowserTest, SubscribeGated) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  ASSERT_TRUE(StartHub());

  uint32_t sub_slot = hub_->emit_ask(
      0, "legion-subscribe-remote",
      Value::make_object(
          {{"uri", Value(std::string(
                "legion://chrome/cdp/Target/events/targetCreated"))},
           {"sub_id", Value(std::string("cf8-gated"))},
           {"cap", Value(ScopedCap("legion://chrome/system/*"))}}));
  Dispatcher::AnswerOutcome sub_ans = Settle(sub_slot);
  ASSERT_TRUE(sub_ans.settled);
  EXPECT_FALSE(sub_ans.ok)
      << "an out-of-cap subscribe must refuse BEFORE registration";
  EXPECT_NE(sub_ans.reason.find("cap-refused"), std::string::npos)
      << sub_ans.reason;
  EXPECT_NE(sub_ans.reason.find("pattern-mismatch"), std::string::npos)
      << sub_ans.reason;

  // The registration must not exist: drive the event, expect NO frames.
  chrome::AddTabAt(browser(), GURL("about:blank"), -1, /*foreground=*/true);
  for (int i = 0; i < 400; ++i) {
    PumpHub();
    Spin();
  }
  EXPECT_TRUE(hub_bs_->events.empty())
      << "a refused subscribe still registered — events crossed the wire";
}

// (4) Design §7's per-delivery clause, pinned: a subscription admitted under
// an EXPIRING cap delivers while the cap lives; past expiry the delivery
// path drops the event and terminates the subscription typed
// (cap-refused:cap-expired) — caps expire, so `now` is re-evaluated at
// delivery, not just at subscribe. RED today: the cap is ignored and events
// keep flowing after expiry.
IN_PROC_BROWSER_TEST_F(AurelianNavCapBrowserTest, SubscribeCapExpiryTerminates) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  ASSERT_TRUE(StartHub());

  const int64_t expires = base::Time::Now().ToTimeT() + 2;
  uint32_t sub_slot = hub_->emit_ask(
      0, "legion-subscribe-remote",
      Value::make_object(
          {{"uri", Value(std::string(
                "legion://chrome/cdp/Target/events/targetCreated"))},
           {"sub_id", Value(std::string("cf8-expiry"))},
           {"cap", Value(ScopedCap("legion://chrome/*", expires))}}));
  Dispatcher::AnswerOutcome sub_ans = Settle(sub_slot);
  ASSERT_TRUE(sub_ans.settled && sub_ans.ok)
      << "a live in-cap subscribe must ack: " << sub_ans.reason;

  // While the cap lives: the event crosses.
  chrome::AddTabAt(browser(), GURL("about:blank"), -1, /*foreground=*/true);
  for (int i = 0; i < 1600 && hub_bs_->events.empty(); ++i) {
    PumpHub();
    Spin();
  }
  ASSERT_FALSE(hub_bs_->events.empty())
      << "the in-cap event must cross while the cap lives";

  // Drain to QUIESCENCE before the expiry wait — phase-1 may deliver more
  // than one frame (late wire arrivals must not be misread as post-expiry
  // deliveries).
  for (size_t last = 0; last != hub_bs_->events.size();) {
    last = hub_bs_->events.size();
    for (int i = 0; i < 100; ++i) {
      PumpHub();
      Spin();
    }
  }

  // Outlive the cap, then drive another event: it must NOT be delivered as
  // an ordinary frame — the delivery path terminates the subscription with
  // the typed cancelled notify instead.
  while (base::Time::Now().ToTimeT() <= expires + 1) {
    base::PlatformThread::Sleep(base::Milliseconds(100));
  }
  const size_t live_frames = hub_bs_->events.size();
  chrome::AddTabAt(browser(), GURL("about:blank"), -1, /*foreground=*/true);
  bool saw_terminal = false;
  for (int i = 0; i < 1600 && !saw_terminal; ++i) {
    PumpHub();
    for (size_t k = live_frames; k < hub_bs_->events.size(); ++k) {
      const Value* body = hub_bs_->events[k].object_get("event");
      const Value* state = body ? body->object_get("state") : nullptr;
      if (state && state->is_string() &&
          state->as_string() == "cancelled") {
        const Value* reason = body->object_get("reason");
        ASSERT_TRUE(reason && reason->is_string());
        EXPECT_NE(reason->as_string().find("cap-expired"),
                  std::string::npos)
            << "the post-expiry termination must be typed: "
            << reason->as_string();
        saw_terminal = true;
      } else {
        ADD_FAILURE() << "an ordinary event frame crossed AFTER cap expiry "
                         "(the per-delivery re-check is absent)";
        return;
      }
    }
    Spin();
  }
  EXPECT_TRUE(saw_terminal)
      << "post-expiry delivery must terminate the subscription typed";

  // The subscription is dead: further events produce NO frames.
  const size_t after_terminal = hub_bs_->events.size();
  chrome::AddTabAt(browser(), GURL("about:blank"), -1, /*foreground=*/true);
  for (int i = 0; i < 400; ++i) {
    PumpHub();
    Spin();
  }
  EXPECT_EQ(hub_bs_->events.size(), after_terminal)
      << "no frames may follow the typed expiry termination";
}

}  // namespace aurelian
