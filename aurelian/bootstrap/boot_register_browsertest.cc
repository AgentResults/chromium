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

#include <string>

#include "base/run_loop.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/platform_thread.h"
#include "base/threading/thread_restrictions.h"
#include "base/time/time.h"
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
      return ValueHandle::make(Value(std::string("ok")));
    }
    return ValueHandle::make_broken("legion://errors/UnknownMessage");
  }
  void tell(std::string_view, const Value&) override {}
  std::string registered;

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

}  // namespace aurelian
