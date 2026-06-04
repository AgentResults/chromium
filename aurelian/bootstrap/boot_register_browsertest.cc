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

#include "base/threading/platform_thread.h"
#include "base/threading/thread_restrictions.h"
#include "base/time/time.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "content/public/test/browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "velite/channel.hpp"
#include "velite/uds_channel.hpp"
#include "velite/uds_listener.hpp"

namespace aurelian {

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

}  // namespace aurelian
