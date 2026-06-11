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
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "aurelian/capability/cap_chain.h"
#include "aurelian/capability/cap_wire.h"

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
using velite::agentspaces::SlotRef;
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
                        [](const std::string&, const std::string&) { return std::string("x"); },
                        /*cap_anchor=*/{}))
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
      [&dispatched](const std::string& p, const std::string&) -> std::string {
        dispatched.store(true);
        return std::string("answer:") + p;  // e.g. answer:system/info
      },
      /*cap_anchor=*/{}));

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
      [](const std::string& p, const std::string&) -> std::string {
        return std::string("answer:") + p;  // e.g. answer:system/info
      },
      /*cap_anchor=*/{}));

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


// ---------------------------------------------------------------------------
// HS-3 (ACM-2w) — the spec-carrying wire (design section 5). RED A at the
// exact seam round-2 review F4 named: AurelianBootstrap's dispatchAt read
// only {uri, verb} (the spec field was never read), NavHandle leaf asks
// discarded the caller's spec, and ChromeDispatchFn was string->string —
// the keystone's "no-arg reads" ceiling was structural, not a test-scope
// accident. The probe ChromeDispatchFn asserts what crosses the seam.
// ---------------------------------------------------------------------------

namespace {

// The same real-UDS + hub-Dispatcher shape as the tests above, factored:
// the spec tests vary only the probe and the ask.
struct SpecWireHarness {
  std::string sock_path;
  velite::UdsListener listener;
  velite::UdsChannel server;
  UdsRegister reg;
  std::shared_ptr<HubBootstrap> hub_bs;
  std::unique_ptr<Dispatcher> hub;
  // ACM-9p: the hub-side caller buffer sized to the wire MTU ON THE HEAP
  // (the G3 caveat — never a 1 MiB stack buffer), so spec-legal large
  // envelopes (64 KB, 1 MiB] can cross the test hub hop too.
  std::unique_ptr<uint8_t[]> buf =
      std::make_unique<uint8_t[]>(velite::VELITE_CHANNEL_MAX_ENVELOPE_BYTES);

  bool Start(ChromeDispatchFn dispatch,
             const std::vector<uint8_t>& cap_anchor = {}) {
    sock_path = TempSock();
    ::unlink(sock_path.c_str());
    if (!listener.listen(sock_path.c_str())) {
      return false;
    }
    if (!reg.Start(sock_path, "chrome", "ed25519:chromeDH",
                   std::move(dispatch), cap_anchor)) {
      return false;
    }
    velite::UdsListener::AcceptOutcome out =
        velite::UdsListener::AcceptOutcome::NoneReady;
    for (int i = 0;
         i < 2000 && out != velite::UdsListener::AcceptOutcome::Accepted;
         ++i) {
      out = listener.accept(server, nullptr);
      base::PlatformThread::Sleep(base::Milliseconds(1));
    }
    if (out != velite::UdsListener::AcceptOutcome::Accepted) {
      return false;
    }
    hub_bs = HubBootstrap::make();
    velite::UdsChannel* raw = &server;
    hub = std::make_unique<Dispatcher>(hub_bs, [raw](const std::string& f) {
      raw->send(reinterpret_cast<const uint8_t*>(f.data()), f.size());
    });
    for (int i = 0; i < 2000 && hub_bs->registered_facet.empty(); ++i) {
      Pump();
      base::PlatformThread::Sleep(base::Milliseconds(1));
    }
    return hub_bs->registered_facet == "chrome";
  }

  void Pump() {
    for (;;) {
      size_t n = 0;
      if (server.recv(buf.get(), velite::VELITE_CHANNEL_MAX_ENVELOPE_BYTES,
                      &n) == velite::ChannelError::OK &&
          n > 0) {
        hub->on_inbound(
            std::string(reinterpret_cast<const char*>(buf.get()), n));
      } else {
        break;
      }
    }
    hub->pump_pending_answers();
  }

  Dispatcher::AnswerOutcome AskSettled(uint32_t slot) {
    Dispatcher::AnswerOutcome o;
    for (int i = 0; i < 4000; ++i) {
      Pump();
      o = hub->answer_outcome(slot);
      if (o.settled) {
        break;
      }
      base::PlatformThread::Sleep(base::Milliseconds(1));
    }
    return o;
  }

  ~SpecWireHarness() {
    reg.Stop();
    ::unlink(sock_path.c_str());
  }
};

}  // namespace

// dispatchAt{uri, verb:"invoke", spec:{expression:"6*7"}} delivers the spec
// to the dispatch seam SERIALIZED (canonical JSON; velite-free seam).
TEST(AurelianUdsRegisterTest, ForwardedDispatchAtCarriesSpecSerialized) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  SpecWireHarness h;
  std::string seen_path;
  std::string seen_spec = "<never-called>";
  ASSERT_TRUE(h.Start(
      [&](const std::string& p, const std::string& s) -> std::string {
        seen_path = p;
        seen_spec = s;
        return std::string("ok");
      }));

  uint32_t slot = h.hub->emit_ask(
      0, "dispatchAt",
      Value::make_object(
          {{"uri",
            Value(std::string("legion://chrome/cdp/Runtime/evaluate"))},
           {"verb", Value(std::string("invoke"))},
           {"spec", Value::make_object(
                        {{"expression", Value(std::string("6*7"))}})}}));
  Dispatcher::AnswerOutcome o = h.AskSettled(slot);

  EXPECT_TRUE(o.settled && o.ok) << o.reason;
  EXPECT_EQ(seen_path, "cdp/Runtime/evaluate/invoke");
  EXPECT_NE(seen_spec.find("expression"), std::string::npos)
      << "the spec did not cross the seam serialized; dispatch saw: "
      << seen_spec;
  EXPECT_NE(seen_spec.find("6*7"), std::string::npos) << seen_spec;
}

// The NavHandle walk carries the caller's spec on leaf asks too —
// getResource(uri).ask("invoke", spec) is the navigable form of the same
// seam (round-2 verification: NavHandle leaf verbs discarded the spec).
TEST(AurelianUdsRegisterTest, NavHandleLeafAskCarriesSpec) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  SpecWireHarness h;
  std::string seen_spec = "<never-called>";
  ASSERT_TRUE(h.Start(
      [&](const std::string&, const std::string& s) -> std::string {
        seen_spec = s;
        return std::string("ok");
      }));

  uint32_t r_slot = h.hub->emit_ask(
      0, "getResource",
      Value::make_object(
          {{"uri",
            Value(std::string("legion://chrome/cdp/Runtime/evaluate"))}}));
  Dispatcher::AnswerOutcome ro = h.AskSettled(r_slot);
  ASSERT_TRUE(ro.settled && ro.ok) << ro.reason;
  ASSERT_TRUE(ro.value.is_slot_ref());

  uint32_t i_slot = h.hub->emit_ask(
      ro.value.as_slot_ref().slot, "invoke",
      Value::make_object({{"expression", Value(std::string("6*7"))}}));
  Dispatcher::AnswerOutcome io = h.AskSettled(i_slot);

  EXPECT_TRUE(io.settled && io.ok) << io.reason;
  EXPECT_NE(seen_spec.find("expression"), std::string::npos)
      << "NavHandle discarded the caller's spec; dispatch saw: " << seen_spec;
  EXPECT_NE(seen_spec.find("6*7"), std::string::npos) << seen_spec;
}

// VALUE-ONLY seam (design section 5, round-5 review M5): a re-parsed
// serialized spec cannot resolve wire slot refs, so a slot-ref-bearing spec
// is REFUSED typed — never silently flattened into a dispatch.
TEST(AurelianUdsRegisterTest, SlotRefBearingSpecRefusedTyped) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  SpecWireHarness h;
  std::atomic<bool> dispatched{false};
  ASSERT_TRUE(h.Start(
      [&](const std::string&, const std::string&) -> std::string {
        dispatched.store(true);
        return std::string("ok");
      }));

  uint32_t slot = h.hub->emit_ask(
      0, "dispatchAt",
      Value::make_object(
          {{"uri",
            Value(std::string("legion://chrome/cdp/Runtime/evaluate"))},
           {"verb", Value(std::string("invoke"))},
           {"spec",
            Value::make_object({{"sneaky", Value(SlotRef{7})}})}}));
  Dispatcher::AnswerOutcome o = h.AskSettled(slot);

  ASSERT_TRUE(o.settled);
  EXPECT_FALSE(o.ok)
      << "a slot-ref-bearing spec must be refused typed; it dispatched";
  EXPECT_NE(o.reason.find("SlotRefInSpec"), std::string::npos) << o.reason;
  EXPECT_FALSE(dispatched.load())
      << "the spec was silently flattened into a dispatch";
}

// ---------------------------------------------------------------------------
// ACM-8 (design section 5 prove-or-build) — the federation cap gate at the
// AurelianBootstrap dispatchAt seam. PROVEN before building: descendant-scoped
// enforcement for deep mirror URIs happens NOWHERE on the proven path — the
// browser side verifies no cap (zero cap_* in aurelian/federation/ pre-slice),
// Agrippa's RegisteredEmbodimentHandle::ask_impl forwards every dispatchAt
// blindly, and Agrippa's mint gate is facet-granularity (the whole
// legion://chrome subtree). So the gate is BUILT here, from the existing
// components: cap_wire chain decode + cap_chain/cap_membrane verify back to
// the operator anchor + cap_predicate enforcement over the FULL target URI.
//
// Contract under test: a dispatchAt frame carrying a `cap` field (serialized
// delegation chain) is admitted iff the chain verifies to the provisioned
// anchor AND its effective predicate admits (verb, uri); refusals are typed
// (`cap-refused:<reason>`) and the dispatch NEVER runs. A capless frame keeps
// the connection's facet-level authority (the status quo the keystone proved;
// Agrippa's mint gate authorized the registration). Real Ed25519 chains, real
// UDS wire, real Dispatchers — no mocks.
// ---------------------------------------------------------------------------

namespace {

PrivKey GateSeed(uint8_t b) {
  PrivKey k{};
  k.fill(b);
  return k;
}

CapLink GateMakeLink(const std::string& cap_id,
                     const std::string& parent,
                     const std::string& predicate,
                     int64_t expires,
                     const PrivKey& issuer_priv,
                     const PubKey& subject_pub) {
  CapLink link;
  link.cap_id = cap_id;
  link.parent_cap_id = parent;
  link.predicate = predicate;
  link.expires = expires;
  link.issuer_pub = PubFromPriv(issuer_priv);
  link.subject_pub = subject_pub;
  EXPECT_TRUE(SignLink(&link, issuer_priv));
  return link;
}

// operator anchor -> controller. The anchor IS the trust root the harness
// provisions onto the seam (the same shape the boot path reads from
// AURELIAN_CAP_ANCHOR).
struct CapGateFixture {
  PrivKey anchor = GateSeed(0x51);
  PrivKey controller = GateSeed(0x52);
  PubKey anchor_pub = PubFromPriv(anchor);
  PubKey controller_pub = PubFromPriv(controller);

  std::vector<uint8_t> AnchorBytes() const {
    return std::vector<uint8_t>(anchor_pub.begin(), anchor_pub.end());
  }

  // A single-link chain: the operator grants the controller `pattern`.
  std::vector<CapLink> Scoped(const std::string& pattern) {
    return {GateMakeLink("root", "", "pattern=" + pattern, 0, anchor,
                         controller_pub)};
  }

  static Value CapField(const std::vector<CapLink>& chain) {
    return Value(SerializeChain(chain));
  }
};

// Emit a dispatchAt{uri, verb, spec?, cap?} and settle it.
Dispatcher::AnswerOutcome DispatchWithCap(SpecWireHarness& h,
                                          const std::string& uri,
                                          const std::string& verb,
                                          const Value& spec,
                                          const Value& cap) {
  std::map<std::string, Value> frame{{"uri", Value(uri)},
                                     {"verb", Value(verb)}};
  if (!spec.is_null()) {
    frame.emplace("spec", spec);
  }
  if (!cap.is_null()) {
    frame.emplace("cap", cap);
  }
  uint32_t slot =
      h.hub->emit_ask(0, "dispatchAt", Value::make_object(std::move(frame)));
  return h.AskSettled(slot);
}

}  // namespace

// Positive control: a cap scoped to targets/tabA admits a dispatch INSIDE
// that subtree and the dispatch reaches the seam with the right path.
TEST(AurelianUdsCapGateTest, ScopedCapAdmitsInSubtreeDispatch) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  CapGateFixture f;
  SpecWireHarness h;
  std::string seen_path = "<never-called>";
  ASSERT_TRUE(h.Start(
      [&](const std::string& p, const std::string&) -> std::string {
        seen_path = p;
        return std::string("ok");
      },
      f.AnchorBytes()));

  Dispatcher::AnswerOutcome o = DispatchWithCap(
      h, "legion://chrome/targets/tabA/cdp/Runtime/evaluate", "invoke",
      Value::make_object({{"expression", Value(std::string("6*7"))}}),
      CapGateFixture::CapField(
          f.Scoped("legion://chrome/targets/tabA/*")));

  EXPECT_TRUE(o.settled && o.ok) << o.reason;
  EXPECT_EQ(seen_path, "targets/tabA/cdp/Runtime/evaluate/invoke");
}

// The base node ITSELF is inside the cap's subtree (the trailing-slash
// canonicalization — a `base/*` pattern admits `base`, but never `baseX`).
TEST(AurelianUdsCapGateTest, ScopedCapAdmitsBaseNodeItself) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  CapGateFixture f;
  SpecWireHarness h;
  std::string seen_path = "<never-called>";
  ASSERT_TRUE(h.Start(
      [&](const std::string& p, const std::string&) -> std::string {
        seen_path = p;
        return std::string("ok");
      },
      f.AnchorBytes()));

  Dispatcher::AnswerOutcome o = DispatchWithCap(
      h, "legion://chrome/targets/tabA", "__getChildren", Value(),
      CapGateFixture::CapField(
          f.Scoped("legion://chrome/targets/tabA/*")));

  EXPECT_TRUE(o.settled && o.ok) << o.reason;
  EXPECT_EQ(seen_path, "targets/tabA/__getChildren");
}

// THE plan RED: the same targets/tabA cap is REFUSED on the sibling target —
// typed, and the dispatch never runs. (Pre-gate this dispatched: the cap
// field was silently ignored — the proven enforcement gap.)
TEST(AurelianUdsCapGateTest, ScopedCapRefusedOnSiblingTarget) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  CapGateFixture f;
  SpecWireHarness h;
  std::atomic<bool> dispatched{false};
  ASSERT_TRUE(h.Start(
      [&](const std::string&, const std::string&) -> std::string {
        dispatched.store(true);
        return std::string("ok");
      },
      f.AnchorBytes()));

  Dispatcher::AnswerOutcome o = DispatchWithCap(
      h, "legion://chrome/targets/tabB/cdp/Runtime/evaluate", "invoke",
      Value::make_object({{"expression", Value(std::string("6*7"))}}),
      CapGateFixture::CapField(
          f.Scoped("legion://chrome/targets/tabA/*")));

  ASSERT_TRUE(o.settled);
  EXPECT_FALSE(o.ok) << "a targets/tabA cap must be REFUSED on tab B";
  EXPECT_NE(o.reason.find("cap-refused"), std::string::npos) << o.reason;
  EXPECT_NE(o.reason.find("pattern-mismatch"), std::string::npos) << o.reason;
  EXPECT_FALSE(dispatched.load())
      << "the out-of-subtree dispatch RAN — the gate is absent";
}

// The plan RED's second half: the targets/tabA cap is refused on prefs/*.
TEST(AurelianUdsCapGateTest, ScopedCapRefusedOnPrefs) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  CapGateFixture f;
  SpecWireHarness h;
  std::atomic<bool> dispatched{false};
  ASSERT_TRUE(h.Start(
      [&](const std::string&, const std::string&) -> std::string {
        dispatched.store(true);
        return std::string("ok");
      },
      f.AnchorBytes()));

  Dispatcher::AnswerOutcome o = DispatchWithCap(
      h, "legion://chrome/prefs/browser.show_home_button", "get", Value(),
      CapGateFixture::CapField(
          f.Scoped("legion://chrome/targets/tabA/*")));

  ASSERT_TRUE(o.settled);
  EXPECT_FALSE(o.ok) << "a targets-scoped cap must be REFUSED on prefs/*";
  EXPECT_NE(o.reason.find("cap-refused"), std::string::npos) << o.reason;
  EXPECT_FALSE(dispatched.load());
}

// The plan's domain-narrow case: a cap based at targets/tabA/cdp/Page
// dispatches Page.* on that tab but is REFUSED on Network.*.
TEST(AurelianUdsCapGateTest, PageScopedCapDispatchesPageRefusesNetwork) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  CapGateFixture f;
  SpecWireHarness h;
  std::string seen_path = "<never-called>";
  std::atomic<int> dispatch_count{0};
  ASSERT_TRUE(h.Start(
      [&](const std::string& p, const std::string&) -> std::string {
        seen_path = p;
        dispatch_count.fetch_add(1);
        return std::string("ok");
      },
      f.AnchorBytes()));

  Value cap = CapGateFixture::CapField(
      f.Scoped("legion://chrome/targets/tabA/cdp/Page/*"));

  Dispatcher::AnswerOutcome page = DispatchWithCap(
      h, "legion://chrome/targets/tabA/cdp/Page/navigate", "invoke",
      Value::make_object({{"url", Value(std::string("about:blank"))}}), cap);
  EXPECT_TRUE(page.settled && page.ok) << page.reason;
  EXPECT_EQ(seen_path, "targets/tabA/cdp/Page/navigate/invoke");
  EXPECT_EQ(dispatch_count.load(), 1);

  Dispatcher::AnswerOutcome net = DispatchWithCap(
      h, "legion://chrome/targets/tabA/cdp/Network/getCookies", "invoke",
      Value(), cap);
  ASSERT_TRUE(net.settled);
  EXPECT_FALSE(net.ok) << "a Page-scoped cap must be REFUSED on Network.*";
  EXPECT_NE(net.reason.find("cap-refused"), std::string::npos) << net.reason;
  EXPECT_EQ(dispatch_count.load(), 1)
      << "the Network dispatch RAN despite the Page-scoped cap";
}

// Keystone leg (f) shape: a VALIDLY-SIGNED cap whose subtree is out of this
// embodiment entirely is refused.
TEST(AurelianUdsCapGateTest, ValidlySignedOutOfSubtreeCapRefused) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  CapGateFixture f;
  SpecWireHarness h;
  std::atomic<bool> dispatched{false};
  ASSERT_TRUE(h.Start(
      [&](const std::string&, const std::string&) -> std::string {
        dispatched.store(true);
        return std::string("ok");
      },
      f.AnchorBytes()));

  Dispatcher::AnswerOutcome o = DispatchWithCap(
      h, "legion://chrome/cdp", "__getChildren", Value(),
      CapGateFixture::CapField(f.Scoped("legion://other/*")));

  ASSERT_TRUE(o.settled);
  EXPECT_FALSE(o.ok);
  EXPECT_NE(o.reason.find("cap-refused"), std::string::npos) << o.reason;
  EXPECT_FALSE(dispatched.load());
}

// Trust: a chain rooted in a DIFFERENT key than the provisioned anchor is
// refused as untrusted, never honored.
TEST(AurelianUdsCapGateTest, WrongAnchorChainRefused) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  CapGateFixture f;
  SpecWireHarness h;
  std::atomic<bool> dispatched{false};
  ASSERT_TRUE(h.Start(
      [&](const std::string&, const std::string&) -> std::string {
        dispatched.store(true);
        return std::string("ok");
      },
      f.AnchorBytes()));

  // Same shape, signed by a key that is NOT the provisioned anchor.
  PrivKey rogue = GateSeed(0x66);
  std::vector<CapLink> chain = {GateMakeLink(
      "root", "", "pattern=legion://chrome/*", 0, rogue, f.controller_pub)};

  Dispatcher::AnswerOutcome o =
      DispatchWithCap(h, "legion://chrome/cdp", "__getChildren", Value(),
                      CapGateFixture::CapField(chain));

  ASSERT_TRUE(o.settled);
  EXPECT_FALSE(o.ok);
  EXPECT_NE(o.reason.find("cap-untrusted-anchor"), std::string::npos)
      << o.reason;
  EXPECT_FALSE(dispatched.load());
}

// A malformed cap field is a typed refusal, never silently ignored (silently
// ignoring it was exactly the pre-gate behavior).
TEST(AurelianUdsCapGateTest, MalformedCapRefusedTyped) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  CapGateFixture f;
  SpecWireHarness h;
  std::atomic<bool> dispatched{false};
  ASSERT_TRUE(h.Start(
      [&](const std::string&, const std::string&) -> std::string {
        dispatched.store(true);
        return std::string("ok");
      },
      f.AnchorBytes()));

  Dispatcher::AnswerOutcome o =
      DispatchWithCap(h, "legion://chrome/cdp", "__getChildren", Value(),
                      Value(std::string("not-a-chain")));

  ASSERT_TRUE(o.settled);
  EXPECT_FALSE(o.ok);
  EXPECT_NE(o.reason.find("cap-refused"), std::string::npos) << o.reason;
  EXPECT_NE(o.reason.find("cap-chain-malformed"), std::string::npos)
      << o.reason;
  EXPECT_FALSE(dispatched.load());
}

// Chain discipline holds at the seam: a delegation that BROADENS its parent
// is refused by the verifier (cap-chain-invalid), not honored at its own
// claimed width.
TEST(AurelianUdsCapGateTest, BroadeningDelegationRefused) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  CapGateFixture f;
  SpecWireHarness h;
  std::atomic<bool> dispatched{false};
  ASSERT_TRUE(h.Start(
      [&](const std::string&, const std::string&) -> std::string {
        dispatched.store(true);
        return std::string("ok");
      },
      f.AnchorBytes()));

  PrivKey delegatee = GateSeed(0x53);
  std::vector<CapLink> chain = {
      GateMakeLink("root", "",
                   "pattern=legion://chrome/targets/tabA/cdp/Page/*", 0,
                   f.anchor, f.controller_pub),
      // The child claims the WHOLE target subtree — a broadening.
      GateMakeLink("d1", "root", "pattern=legion://chrome/targets/tabA/*", 0,
                   f.controller, PubFromPriv(delegatee)),
  };

  Dispatcher::AnswerOutcome o = DispatchWithCap(
      h, "legion://chrome/targets/tabA/cdp/Network/getCookies", "invoke",
      Value(), CapGateFixture::CapField(chain));

  ASSERT_TRUE(o.settled);
  EXPECT_FALSE(o.ok);
  EXPECT_NE(o.reason.find("cap-chain-invalid"), std::string::npos)
      << o.reason;
  EXPECT_FALSE(dispatched.load());
}

// Expiry is enforced at the seam.
TEST(AurelianUdsCapGateTest, ExpiredCapRefused) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  CapGateFixture f;
  SpecWireHarness h;
  std::atomic<bool> dispatched{false};
  ASSERT_TRUE(h.Start(
      [&](const std::string&, const std::string&) -> std::string {
        dispatched.store(true);
        return std::string("ok");
      },
      f.AnchorBytes()));

  std::vector<CapLink> chain = {
      GateMakeLink("root", "", "pattern=legion://chrome/*,expires=1000",
                   /*expires=*/1000, f.anchor, f.controller_pub)};

  Dispatcher::AnswerOutcome o =
      DispatchWithCap(h, "legion://chrome/cdp", "__getChildren", Value(),
                      CapGateFixture::CapField(chain));

  ASSERT_TRUE(o.settled);
  EXPECT_FALSE(o.ok);
  EXPECT_NE(o.reason.find("cap-expired"), std::string::npos) << o.reason;
  EXPECT_FALSE(dispatched.load());
}

// Fail-closed: a presented cap with NO provisioned anchor is refused — the
// seam trusts nothing it cannot verify (it never vouches for itself).
TEST(AurelianUdsCapGateTest, CapWithNoProvisionedAnchorRefused) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  CapGateFixture f;
  SpecWireHarness h;
  std::atomic<bool> dispatched{false};
  ASSERT_TRUE(h.Start(
      [&](const std::string&, const std::string&) -> std::string {
        dispatched.store(true);
        return std::string("ok");
      } /* no anchor */));

  Dispatcher::AnswerOutcome o = DispatchWithCap(
      h, "legion://chrome/cdp", "__getChildren", Value(),
      CapGateFixture::CapField(f.Scoped("legion://chrome/*")));

  ASSERT_TRUE(o.settled);
  EXPECT_FALSE(o.ok);
  EXPECT_NE(o.reason.find("cap-untrusted-anchor"), std::string::npos)
      << o.reason;
  EXPECT_FALSE(dispatched.load());
}

// ---------------------------------------------------------------------------
// ACM-9p (design section 8) — the fork's caller-buffer MTU hop. The fixed
// channel (VELITE_CHANNEL_MAX_ENVELOPE_BYTES, heap rx staging, typed
// MessageTooLarge) delivers any spec-legal envelope; the BINDING constraint
// left is the fork serve loop's caller buffer (kRxBuffer). RED 1: a ~900 KB
// spec-legal round-trip wedges on the 64 KB caller buffer — a hang, so the
// harness treats timeout as failure. RED 2: an envelope ABOVE the MTU must be
// refused TYPED at the wire (the session layer answers the channel's
// MessageTooLarge with CLOSE{frame-too-large}), never silently ignored.
// ---------------------------------------------------------------------------

// RED 1 — spec-legal frames cross: ~900 KB spec in, ~900 KB reply out, both
// over the real register UDS (above every 64 KB wedge, legal under the MTU).
TEST(AurelianUdsMtuTest, SpecLegalLargeEnvelopeCrossesBothDirections) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  SpecWireHarness h;
  std::string seen_path;
  ASSERT_TRUE(h.Start(
      [&](const std::string& p, const std::string& s) -> std::string {
        seen_path = p;
        return s;  // echo the serialized spec: the reply leg carries it back
      }));

  const std::string big(900 * 1024, 'a');
  uint32_t slot = h.hub->emit_ask(
      0, "dispatchAt",
      Value::make_object(
          {{"uri", Value(std::string("legion://chrome/cdp/Page/probe"))},
           {"verb", Value(std::string("invoke"))},
           {"spec", Value::make_object({{"payload", Value(big)}})}}));
  Dispatcher::AnswerOutcome o = h.AskSettled(slot);

  ASSERT_TRUE(o.settled)
      << "the ~900 KB round-trip HUNG — a 64 KB caller-buffer wedge "
         "(timeout == failure per the plan)";
  ASSERT_TRUE(o.ok) << o.reason;
  EXPECT_EQ(seen_path, "cdp/Page/probe/invoke");
  ASSERT_TRUE(o.value.is_string());
  EXPECT_GE(o.value.as_string().size(), big.size())
      << "the reply leg dropped the large payload";
  EXPECT_NE(o.value.as_string().find("aaaa"), std::string::npos);
}

// RED 2 — the bound is enforced typed at the wire: one raw envelope of
// MTU+1 bytes is answered with CLOSE{frame-too-large} (the hub dispatcher
// observes the close), and the dispatch layer never sees it.
TEST(AurelianUdsMtuTest, AboveMtuEnvelopeRefusedTypedClose) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  SpecWireHarness h;
  std::atomic<bool> dispatched{false};
  ASSERT_TRUE(h.Start(
      [&](const std::string&, const std::string&) -> std::string {
        dispatched.store(true);
        return std::string("ok");
      }));

  std::vector<uint8_t> oversize(velite::VELITE_CHANNEL_MAX_ENVELOPE_BYTES + 1,
                                'x');
  ASSERT_EQ(h.server.send(oversize.data(), oversize.size()),
            velite::ChannelError::OK);

  bool closed = false;
  for (int i = 0; i < 4000 && !closed; ++i) {
    h.Pump();
    closed = h.hub->closed();
    base::PlatformThread::Sleep(base::Milliseconds(1));
  }
  EXPECT_TRUE(closed)
      << "an above-MTU envelope must be refused TYPED "
         "(CLOSE{frame-too-large}) — it was silently ignored";
  EXPECT_FALSE(dispatched.load());
}

// The status-quo pin: a CAPLESS dispatch keeps the connection's facet-level
// authority (Agrippa's mint gate authorized the registration — the proven
// keystone behavior). The gate narrows presented caps; it does not invent a
// new requirement on the existing path.
TEST(AurelianUdsCapGateTest, CaplessDispatchKeepsFacetAuthority) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  CapGateFixture f;
  SpecWireHarness h;
  std::string seen_path = "<never-called>";
  ASSERT_TRUE(h.Start(
      [&](const std::string& p, const std::string&) -> std::string {
        seen_path = p;
        return std::string("ok");
      },
      f.AnchorBytes()));

  Dispatcher::AnswerOutcome o = DispatchWithCap(
      h, "legion://chrome/system", "info", Value(), Value());

  EXPECT_TRUE(o.settled && o.ok) << o.reason;
  EXPECT_EQ(seen_path, "system/info");
}

}  // namespace aurelian
