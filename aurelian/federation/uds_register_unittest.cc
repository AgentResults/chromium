// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C9 RED-first: the UDS register-in client (uds_register.h) dials a
// REAL velite UdsListener (an Agrippa stand-in), sends the shipped update{Mount}
// register frame, and serves forwarded dispatchAt asks — the machine-federation
// pattern. Real substrate Dispatchers on both sides over a real kernel UDS; no
// mocks. (The full real-agrippa-daemon + real-ChromeRoot SIT is the closure.)

#include "aurelian/federation/uds_register.h"

#include <unistd.h>

#include <atomic>
#include <memory>
#include <string>

#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "base/threading/thread_restrictions.h"
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
using velite::agentspaces::Value;
using velite::agentspaces::ValueHandle;
using velite::agentspaces::StateKind;
using velite::agentspaces::wire::Dispatcher;

// The hub side (Agrippa stand-in): slot-0 bootstrap that records a child's
// update{Mount} and answers ok — exactly what Agrippa's ChildEdge does.
class HubBootstrap : public Handle {
 public:
  static std::shared_ptr<HubBootstrap> make() {
    return std::shared_ptr<HubBootstrap>(new HubBootstrap());
  }
  StateKind state_kind() const override { return StateKind::ResolvedValue; }
  const Value& resolved_value() const override { return self_value_; }
  std::shared_ptr<Handle> resolved_handle() const override { return nullptr; }
  std::string_view broken_reason() const override { return ""; }
  std::shared_ptr<Handle> ask_impl(std::string_view msg,
                                   const Value& spec) override {
    if (msg == "update") {
      const Value* name = spec.is_object() ? spec.object_get("name") : nullptr;
      if (name && name->is_string()) {
        registered_facet = name->as_string();
      }
      return ValueHandle::make(Value(std::string("legion://") +
                                     (name ? name->as_string() : "")));
    }
    return ValueHandle::make_broken("legion://errors/UnknownMessage");
  }
  void tell(std::string_view, const Value&) override {}
  std::string registered_facet;

 private:
  Value self_value_{std::string("hub")};
};

std::string TempSock() {
  return std::string("/tmp/aurelian-c9-") + std::to_string(::getpid()) + ".sock";
}

}  // namespace

// The register client dials a real UDS + sends the update{Mount} register frame.
TEST(AurelianUdsRegisterTest, DialsAndSendsRegisterMountFrame) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  const std::string path = TempSock();
  ::unlink(path.c_str());

  velite::UdsListener listener;
  ASSERT_TRUE(listener.listen(path.c_str())) << "listener bind failed";

  UdsRegister reg;
  ASSERT_TRUE(reg.Start(path, "chrome", "ed25519:chromeDH",
                        [](const std::string&) { return std::string("x"); }))
      << "Start must connect to the UDS";

  // Accept the dialed connection + read the first frame (the register handshake).
  velite::UdsChannel server;
  velite::UdsListener::AcceptOutcome out =
      velite::UdsListener::AcceptOutcome::NoneReady;
  for (int i = 0;
       i < 2000 && out != velite::UdsListener::AcceptOutcome::Accepted; ++i) {
    out = listener.accept(server, nullptr);
    if (out == velite::UdsListener::AcceptOutcome::NoneReady) {
      base::PlatformThread::Sleep(base::Milliseconds(1));
    }
  }
  ASSERT_EQ(out, velite::UdsListener::AcceptOutcome::Accepted);

  std::string frame;
  uint8_t buf[65536];
  for (int i = 0; i < 2000 && frame.empty(); ++i) {
    size_t n = 0;
    if (server.recv(buf, sizeof(buf), &n) == velite::ChannelError::OK && n > 0) {
      frame.assign(reinterpret_cast<const char*>(buf), n);
    } else {
      base::PlatformThread::Sleep(base::Milliseconds(1));
    }
  }
  reg.Stop();
  ::unlink(path.c_str());

  EXPECT_NE(frame.find("update"), std::string::npos) << frame;
  EXPECT_NE(frame.find("Mount"), std::string::npos) << frame;
  EXPECT_NE(frame.find("chrome"), std::string::npos) << frame;
}

// A forwarded dispatchAt{uri,verb} resolves through the register-in to the
// injected ChromeDispatchFn and the value round-trips over the real UDS.
TEST(AurelianUdsRegisterTest, ForwardedDispatchAtResolvesViaDispatch) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  const std::string path = TempSock();
  ::unlink(path.c_str());

  velite::UdsListener listener;
  ASSERT_TRUE(listener.listen(path.c_str()));

  std::atomic<bool> dispatched{false};
  UdsRegister reg;
  ASSERT_TRUE(reg.Start(
      path, "chrome", "ed25519:chromeDH",
      [&dispatched](const std::string& p) -> std::string {
        dispatched.store(true);
        return std::string("answer:") + p;  // e.g. answer:system/info
      }));

  velite::UdsChannel server;
  velite::UdsListener::AcceptOutcome out =
      velite::UdsListener::AcceptOutcome::NoneReady;
  for (int i = 0;
       i < 2000 && out != velite::UdsListener::AcceptOutcome::Accepted; ++i) {
    out = listener.accept(server, nullptr);
    base::PlatformThread::Sleep(base::Milliseconds(1));
  }
  ASSERT_EQ(out, velite::UdsListener::AcceptOutcome::Accepted);

  velite::UdsChannel* raw = &server;
  auto hub_bs = HubBootstrap::make();
  Dispatcher hub(hub_bs, [raw](const std::string& f) {
    raw->send(reinterpret_cast<const uint8_t*>(f.data()), f.size());
  });

  // Pump the hub side until the child's register frame is dispatched.
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
  for (int i = 0; i < 2000 && hub_bs->registered_facet.empty(); ++i) {
    pump_hub();
    base::PlatformThread::Sleep(base::Milliseconds(1));
  }
  EXPECT_EQ(hub_bs->registered_facet, "chrome");

  // Forward a leaf ask: dispatchAt{uri:legion://chrome/system, verb:info}.
  uint32_t slot = hub.emit_ask(
      0, "dispatchAt",
      Value::make_object(
          {{"uri", Value(std::string("legion://chrome/system"))},
           {"verb", Value(std::string("info"))}}));
  Dispatcher::AnswerOutcome o;
  for (int i = 0; i < 4000; ++i) {
    pump_hub();
    o = hub.answer_outcome(slot);
    if (o.settled) {
      break;
    }
    base::PlatformThread::Sleep(base::Milliseconds(1));
  }
  reg.Stop();
  ::unlink(path.c_str());

  EXPECT_TRUE(o.settled && o.ok) << "dispatchAt must settle over the UDS";
  EXPECT_TRUE(dispatched.load());
  EXPECT_TRUE(o.value.is_string() && o.value.as_string() == "answer:system/info")
      << (o.value.is_string() ? o.value.as_string() : "<not-string>");
}

// AU-NAV-HANDLE: getResource{uri} returns a NAVIGABLE child handle (slot-
// exported over the wire), so a controller WALKS the tree —
// peer.getResource(legion://chrome/system).ask("info") — instead of only the
// flat dispatchAt leaf. The returned slot-ref is asked with "info" and the leaf
// value round-trips back over the same real UDS.
TEST(AurelianUdsRegisterTest, ForwardedGetResourceReturnsNavigableChild) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  const std::string path = TempSock();
  ::unlink(path.c_str());

  velite::UdsListener listener;
  ASSERT_TRUE(listener.listen(path.c_str()));

  UdsRegister reg;
  ASSERT_TRUE(reg.Start(
      path, "chrome", "ed25519:chromeDH",
      [](const std::string& p) -> std::string {
        return std::string("answer:") + p;  // e.g. answer:system/info
      }));

  velite::UdsChannel server;
  velite::UdsListener::AcceptOutcome out =
      velite::UdsListener::AcceptOutcome::NoneReady;
  for (int i = 0;
       i < 2000 && out != velite::UdsListener::AcceptOutcome::Accepted; ++i) {
    out = listener.accept(server, nullptr);
    base::PlatformThread::Sleep(base::Milliseconds(1));
  }
  ASSERT_EQ(out, velite::UdsListener::AcceptOutcome::Accepted);

  velite::UdsChannel* raw = &server;
  auto hub_bs = HubBootstrap::make();
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
  for (int i = 0; i < 2000 && hub_bs->registered_facet.empty(); ++i) {
    pump_hub();
    base::PlatformThread::Sleep(base::Milliseconds(1));
  }
  ASSERT_EQ(hub_bs->registered_facet, "chrome");

  // 1) getResource{uri:legion://chrome/system} -> a navigable child (slot-ref).
  uint32_t r_slot = hub.emit_ask(
      0, "getResource",
      Value::make_object(
          {{"uri", Value(std::string("legion://chrome/system"))}}));
  Dispatcher::AnswerOutcome ro;
  for (int i = 0; i < 4000; ++i) {
    pump_hub();
    ro = hub.answer_outcome(r_slot);
    if (ro.settled) {
      break;
    }
    base::PlatformThread::Sleep(base::Milliseconds(1));
  }
  ASSERT_TRUE(ro.settled && ro.ok) << "getResource must settle";
  ASSERT_TRUE(ro.value.is_slot_ref())
      << "getResource must return a navigable child handle (slot-ref), not a "
         "value";

  // 2) ask the returned child slot "info" -> the leaf value, proving the walk.
  uint32_t child_slot = ro.value.as_slot_ref().slot;
  uint32_t i_slot = hub.emit_ask(child_slot, "info", Value{});
  Dispatcher::AnswerOutcome io;
  for (int i = 0; i < 4000; ++i) {
    pump_hub();
    io = hub.answer_outcome(i_slot);
    if (io.settled) {
      break;
    }
    base::PlatformThread::Sleep(base::Milliseconds(1));
  }
  reg.Stop();
  ::unlink(path.c_str());

  EXPECT_TRUE(io.settled && io.ok)
      << "ask against the navigable child must settle over the UDS";
  EXPECT_TRUE(io.value.is_string() &&
              io.value.as_string() == "answer:system/info")
      << "getResource(uri).info() must walk to the real leaf; got "
      << (io.value.is_string() ? io.value.as_string() : "<not-string>");
}

}  // namespace aurelian
