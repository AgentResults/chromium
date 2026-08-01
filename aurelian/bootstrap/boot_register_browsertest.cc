// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C9 boot-wiring (RED-first): the machine-federation register-in must
// run in the actual browser BOOT PATH (browser_main_extra PostCreateThreads),
// not just in a unit test — otherwise registration is tested but never live in
// production. This boots a REAL browser pointed (via AGRIPPA_UDS_PATH) at a REAL
// velite UdsListener standing in for Agrippa, and asserts the boot dialed it and
// sent the update{Mount, name:"chrome"} register frame. No mocks.

#include <unistd.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "aurelian/capability/cap_chain.h"
#include "aurelian/capability/cap_wire.h"
#include "aurelian/conformance/facet_claims.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/platform_thread.h"
#include "base/threading/thread_restrictions.h"
#include "base/time/time.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "content/public/test/browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "velite/agentspaces-wire/dispatcher.hpp"
#include "velite/agentspaces-wire/handle.hpp"
#include "velite/agentspaces-wire/marshal.hpp"
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

// The hub side (Agrippa stand-in): slot-0 bootstrap that records the child's
// update{Mount} + answers ok — what Agrippa's ChildEdge does.
class HubBootstrap : public Handle {
 public:
  static std::shared_ptr<HubBootstrap> make() {
    return std::shared_ptr<HubBootstrap>(new HubBootstrap());
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
      // CF-4: record the register frame's childDestHash param (the real
      // identity pin — [EMBODIMENT-REGISTERED-CHILD-OWN-IDENTITY]).
      const Value* params =
          spec.is_object() ? spec.object_get("params") : nullptr;
      if (params && params->is_object()) {
        const Value* dh = params->object_get("childDestHash");
        if (dh && dh->is_string()) {
          child_dest_hash = dh->as_string();
        }
      }
      return ValueHandle::make(Value(std::string("ok")));
    }
    return ValueHandle::make_broken("legion://errors/UnknownMessage");
  }
  // The register-in wire carries the facet manifest on the sessions.md §2a
  // propose-session ENVELOPE, which the Dispatcher's session layer consumes —
  // it never reaches a bootstrap handle. The legacy proactive __handshake TELL
  // this once watched for is RETIRED (uds_register.cc says so), so watching for
  // it meant asserting a frame nothing sends. The manifest is captured off the
  // raw wire in the test body instead.
  void tell(std::string_view, const Value&) override {}
  std::string registered;
  std::string child_dest_hash;

 private:
  Value self_{std::string("hub")};
};

}  // namespace

class AurelianBootRegisterBrowserTest : public InProcessBrowserTest {
 protected:
  // Stand up the Agrippa-stand-in UDS + point the browser's boot register at it
  // BEFORE the browser process boots (PostCreateThreads dials it).
  void SetUpInProcessBrowserTestFixture() override {
    base::ScopedAllowBlockingForTesting allow_blocking;
    sock_ = std::string("/tmp/aurelian-boot-") + std::to_string(::getpid()) +
            ".sock";
    ::unlink(sock_.c_str());
    ASSERT_TRUE(listener_.listen(sock_.c_str())) << "agrippa-stand-in bind";
    ::setenv("AGRIPPA_UDS_PATH", sock_.c_str(), /*overwrite=*/1);
  }

  void TearDownInProcessBrowserTestFixture() override {
    ::unlink(sock_.c_str());
  }

  std::string sock_;
  velite::UdsListener listener_;
};

IN_PROC_BROWSER_TEST_F(AurelianBootRegisterBrowserTest, BootRegistersChromeFacet) {
  base::ScopedAllowBlockingForTesting allow_blocking;

  // The browser has booted; PostCreateThreads ran UdsRegister::Start against the
  // test sock. Accept the dialed connection.
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
  ASSERT_EQ(out, velite::UdsListener::AcceptOutcome::Accepted)
      << "boot must DIAL the Agrippa UDS (UdsRegister::Start in the boot path)";

  // Read the register frame the boot sent.
  std::string frame;
  uint8_t buf[65536];
  for (int i = 0; i < 4000 && frame.empty(); ++i) {
    size_t n = 0;
    if (server.recv(buf, sizeof(buf), &n) == velite::ChannelError::OK && n > 0) {
      frame.assign(reinterpret_cast<const char*>(buf), n);
    } else {
      base::PlatformThread::Sleep(base::Milliseconds(1));
    }
  }
  EXPECT_NE(frame.find("update"), std::string::npos) << frame;
  EXPECT_NE(frame.find("Mount"), std::string::npos) << frame;
  EXPECT_NE(frame.find("chrome"), std::string::npos)
      << "boot must register facet \"chrome\": " << frame;
}

// C9-forward-threading: a controller's forwarded dispatchAt must reach the
// SEALED legion://chrome/ root and return its REAL answer — not a refusal. The
// forward arrives on UdsRegister's serve thread; browser handles are UI-thread
// affine, so the dispatch must HOP to the UI thread. (RED before the hop: the
// forward returns "broken:ui-hop-pending".)
IN_PROC_BROWSER_TEST_F(AurelianBootRegisterBrowserTest,
                       BootForwardsDispatchAtToSealedRoot) {
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
  // Spin the UI message loop briefly so Aurelian's serve-thread UI-hop tasks run.
  auto spin = [&]() {
    base::RunLoop loop;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, loop.QuitClosure(), base::Milliseconds(5));
    loop.Run();
  };

  // Drain the register handshake.
  for (int i = 0; i < 800 && hub_bs->registered.empty(); ++i) {
    pump_hub();
    spin();
  }
  ASSERT_EQ(hub_bs->registered, "chrome");

  // Forward a leaf ask: dispatchAt{uri:legion://chrome, verb:__getIdentity}.
  uint32_t slot = hub.emit_ask(
      0, "dispatchAt",
      Value::make_object(
          {{"uri", Value(std::string("legion://chrome"))},
           {"verb", Value(std::string("__getIdentity"))}}));
  Dispatcher::AnswerOutcome ans;
  for (int i = 0; i < 800; ++i) {
    pump_hub();
    ans = hub.answer_outcome(slot);
    if (ans.settled) {
      break;
    }
    spin();
  }
  EXPECT_TRUE(ans.settled && ans.ok) << "forward must settle over the UDS";
  EXPECT_TRUE(ans.value.is_string() &&
              ans.value.as_string() == "legion://chrome/")
      << "forwarded dispatchAt must reach the SEALED root (UI-hop), got: "
      << (ans.value.is_string() ? ans.value.as_string() : "<not-string>");
}

// ---------------------------------------------------------------------------
// CF-4 — slot-0 unification on the register-in (conformant-federation design
// §4): the production wire serves the SAME unified surface the corpus just
// certified. Three pins, RED-first.
// ---------------------------------------------------------------------------

// (1) The slot-0 surface answers the CANONICAL bootstrap verbs (ping) AND the
// kept dispatchAt facade (the keystone wire shape, unchanged) over the SAME
// register-in UDS. RED today: AurelianBootstrap answers dispatchAt but
// rejects ping with UnknownMessage.
IN_PROC_BROWSER_TEST_F(AurelianBootRegisterBrowserTest,
                       SlotZeroAnswersCanonicalSurface) {
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

  auto settle = [&](uint32_t slot) {
    Dispatcher::AnswerOutcome ans;
    for (int i = 0; i < 800; ++i) {
      pump_hub();
      ans = hub.answer_outcome(slot);
      if (ans.settled) {
        break;
      }
      spin();
    }
    return ans;
  };

  // The canonical corpus verb on the PRODUCTION wire.
  Dispatcher::AnswerOutcome pong = settle(hub.emit_ask(0, "ping", Value()));
  ASSERT_TRUE(pong.settled) << "ping must settle over the register-in wire";
  EXPECT_TRUE(pong.ok) << pong.reason;
  EXPECT_TRUE(pong.value.is_string() && pong.value.as_string() == "pong")
      << "slot 0 must answer the canonical surface (design §4.1)";

  // The kept facade, unchanged keystone wire shape.
  Dispatcher::AnswerOutcome ident = settle(hub.emit_ask(
      0, "dispatchAt",
      Value::make_object({{"uri", Value(std::string("legion://chrome"))},
                          {"verb", Value(std::string("__getIdentity"))}})));
  ASSERT_TRUE(ident.settled && ident.ok) << ident.reason;
  EXPECT_TRUE(ident.value.is_string() &&
              ident.value.as_string() == "legion://chrome/")
      << "dispatchAt (the FOLD facade) must keep the keystone wire shape";
}

// (2) The register frame presents the browser's REAL destHash — the seeded
// Ed25519 identity (32-hex first16(SHA-512(pub))), persisted so it is stable
// across restarts — never the "aurelian-browser" literal.
// [EMBODIMENT-REGISTERED-CHILD-OWN-IDENTITY]. RED today: the literal.
IN_PROC_BROWSER_TEST_F(AurelianBootRegisterBrowserTest,
                       RegisterPresentsRealDestHash) {
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
  auto spin = [&]() {
    base::RunLoop loop;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, loop.QuitClosure(), base::Milliseconds(5));
    loop.Run();
  };
  for (int i = 0; i < 800 && hub_bs->child_dest_hash.empty(); ++i) {
    pump_hub();
    spin();
  }
  ASSERT_FALSE(hub_bs->child_dest_hash.empty())
      << "the register Mount params must carry childDestHash";
  EXPECT_NE(hub_bs->child_dest_hash, "aurelian-browser")
      << "the literal must be replaced by the real identity";
  EXPECT_EQ(hub_bs->child_dest_hash.size(), 32u)
      << "destHash = hex(first16(SHA-512(pub))), got: "
      << hub_bs->child_dest_hash;
  EXPECT_TRUE(hub_bs->child_dest_hash.find_first_not_of(
                  "0123456789abcdef") == std::string::npos)
      << "destHash must be lowercase hex, got: " << hub_bs->child_dest_hash;
}

// (3) The __handshake facet manifest follows the register ask on THIS wire —
// the SAME computed set the conformance serve emits (design §3.2/§3.5: the
// second pre-flight artefact). RED today: no manifest on the register wire.
IN_PROC_BROWSER_TEST_F(AurelianBootRegisterBrowserTest,
                       HandshakeManifestOnRegisterWire) {
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
  auto hub_bs = HubBootstrap::make();
  Dispatcher hub(hub_bs, [raw](const std::string& f) {
    raw->send(reinterpret_cast<const uint8_t*>(f.data()), f.size());
  });
  uint8_t buf[65536];
  // facets.md §6a: the connector advertises its manifest inline on the
  // propose-session envelope (sessions.md §2a.2). Captured HERE, off the raw
  // frame, because the session layer handles it before any handle sees it.
  Value proposed_manifest;
  bool has_manifest = false;
  auto pump_hub = [&]() {
    for (;;) {
      size_t n = 0;
      if (server.recv(buf, sizeof(buf), &n) == velite::ChannelError::OK &&
          n > 0) {
        const std::string frame(reinterpret_cast<const char*>(buf), n);
        if (!has_manifest) {
          if (auto env = velite::agentspaces::marshal::decode(frame)) {
            const Value* op = env->is_object() ? env->object_get("op") : nullptr;
            if (op && op->is_string() && op->as_string() == "propose-session") {
              if (const Value* m = env->object_get("manifest")) {
                proposed_manifest = *m;
                has_manifest = true;
              }
            }
          }
        }
        hub.on_inbound(frame);
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
  for (int i = 0; i < 800 && !has_manifest; ++i) {
    pump_hub();
    spin();
  }
  ASSERT_TRUE(has_manifest)
      << "a propose-session carrying this peer's facet manifest must follow "
         "the register ask (facets.md §6a + sessions.md §2a.2)";

  // The manifest carries claimed wire facets under `supports`, as
  // {facet, versionMin, versionMax} entries (Dispatcher::build_own_facet_manifest_)
  // — not a flat `wireFacets` string array, which is the shape the retired
  // __handshake payload used.
  std::set<std::string> wire;
  if (const Value* arr = proposed_manifest.object_get("supports");
      arr && arr->is_array()) {
    for (const Value& entry : arr->as_array()) {
      if (!entry.is_object()) continue;
      if (const Value* f = entry.object_get("facet"); f && f->is_string()) {
        wire.insert(f->as_string());
      }
    }
  }
  const std::vector<std::string>& claimed = aurelian_claimed_wire_facets();
  EXPECT_EQ(wire, std::set<std::string>(claimed.begin(), claimed.end()))
      << "the register-in wire must emit the SAME computed manifest the "
         "conformance serve emits (design §3.5 pre-flight artefact 2)";
}

// ---------------------------------------------------------------------------
// ACM-8 — the federation cap gate, LIVE in the boot path: the operator anchor
// published via AURELIAN_CAP_ANCHOR must reach the dispatchAt seam (the same
// anchor the renderer membranes get), and a scoped cap presented over the
// real boot UDS must be enforced against the REAL sealed root — in-subtree
// admitted, out-of-subtree refused typed BEFORE the dispatch runs.
// ---------------------------------------------------------------------------

class AurelianBootCapGateBrowserTest : public AurelianBootRegisterBrowserTest {
 protected:
  void SetUpInProcessBrowserTestFixture() override {
    AurelianBootRegisterBrowserTest::SetUpInProcessBrowserTestFixture();
    // Publish the operator anchor the way Agrippa does at spawn — the boot
    // path reads it in PostCreateThreads (ResolveCapAnchor) and must thread
    // it to BOTH membranes (renderer provisioner + the federation seam).
    anchor_priv_.fill(0xA7);
    PubKey pub = PubFromPriv(anchor_priv_);
    ::setenv("AURELIAN_CAP_ANCHOR", base::HexEncode(pub).c_str(),
             /*overwrite=*/1);
  }

  void TearDownInProcessBrowserTestFixture() override {
    ::unsetenv("AURELIAN_CAP_ANCHOR");
    AurelianBootRegisterBrowserTest::TearDownInProcessBrowserTestFixture();
  }

  // A single-link operator->controller chain scoped to `pattern`.
  std::string ScopedCap(const std::string& pattern) {
    PrivKey controller{};
    controller.fill(0xA8);
    CapLink link;
    link.cap_id = "root";
    link.parent_cap_id = "";
    link.predicate = "pattern=" + pattern;
    link.expires = 0;
    link.issuer_pub = PubFromPriv(anchor_priv_);
    link.subject_pub = PubFromPriv(controller);
    EXPECT_TRUE(SignLink(&link, anchor_priv_));
    return SerializeChain({link});
  }

  PrivKey anchor_priv_{};
};

IN_PROC_BROWSER_TEST_F(AurelianBootCapGateBrowserTest,
                       BootGateEnforcesScopedCapOnSealedRoot) {
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

  auto dispatch_at = [&](const std::string& uri, const std::string& verb,
                         const std::string& cap) {
    std::map<std::string, Value> frame{{"uri", Value(uri)},
                                       {"verb", Value(verb)}};
    if (!cap.empty()) {
      frame.emplace("cap", Value(cap));
    }
    uint32_t slot =
        hub.emit_ask(0, "dispatchAt", Value::make_object(std::move(frame)));
    Dispatcher::AnswerOutcome ans;
    for (int i = 0; i < 800; ++i) {
      pump_hub();
      ans = hub.answer_outcome(slot);
      if (ans.settled) {
        break;
      }
      spin();
    }
    return ans;
  };

  // (a) An in-subtree cap (the whole chrome subtree) is ADMITTED and the ask
  // reaches the REAL sealed root.
  Dispatcher::AnswerOutcome ok_ans = dispatch_at(
      "legion://chrome", "__getIdentity", ScopedCap("legion://chrome/*"));
  EXPECT_TRUE(ok_ans.settled && ok_ans.ok) << ok_ans.reason;
  EXPECT_TRUE(ok_ans.value.is_string() &&
              ok_ans.value.as_string() == "legion://chrome/")
      << "cap-admitted dispatch must reach the sealed root, got: "
      << (ok_ans.value.is_string() ? ok_ans.value.as_string()
                                   : "<not-string>");

  // (b) A cap scoped to a (nonexistent) target subtree must be REFUSED on
  // prefs — typed, with the REAL pref never read. (RED pre-gate: the cap is
  // silently ignored and the live pref value comes back.)
  Dispatcher::AnswerOutcome refused = dispatch_at(
      "legion://chrome/prefs/browser.show_home_button", "get",
      ScopedCap("legion://chrome/targets/no-such-target/*"));
  ASSERT_TRUE(refused.settled);
  EXPECT_FALSE(refused.ok)
      << "the scoped cap was honored OUTSIDE its subtree; value="
      << (refused.value.is_string() ? refused.value.as_string()
                                    : "<not-string>");
  EXPECT_NE(refused.reason.find("cap-refused"), std::string::npos)
      << refused.reason;
}

}  // namespace aurelian
