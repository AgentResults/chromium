// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C9 — the bring-up WSS peer: a remote caller reaches the Chromium
// domain (legion://chrome/) over a real WebSocket and gets a real reply,
// dispatched through the Velite root handle. Loopback (one machine); the
// cross-host + MCP gateway SIT is sequenced after the Agrippa control-plane.

#include "aurelian/federation/wss_peer.h"

#include <atomic>
#include <string>
#include <thread>

#include "base/process/process.h"
#include "base/run_loop.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/thread_restrictions.h"
#include "base/time/time.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "content/public/test/browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace aurelian {

using AurelianWssPeerBrowserTest = InProcessBrowserTest;

IN_PROC_BROWSER_TEST_F(AurelianWssPeerBrowserTest, RemotePeerReachesChromeRoot) {
  // Serve the Chromium domain on an OS-assigned port.
  WssPeer peer;
  uint16_t port = peer.Start(0, &DispatchChromeRoot);
  ASSERT_NE(port, 0u) << "peer failed to bind";

  // Real leaves (system/info) are UI-thread-affine, so DispatchChromeRoot hops
  // to the UI thread. The client's connect/recv BLOCK, so they must run OFF the
  // UI thread — otherwise the UI thread is stuck in Recv and the hop deadlocks.
  // Run the client on a worker thread; spin the UI loop until it finishes.
  std::string id_reply, info_reply;
  std::atomic<bool> done{false};
  std::thread client_thread([&] {
    base::ScopedAllowBlockingForTesting allow_blocking;
    WssClient client;
    if (client.Connect("127.0.0.1", port)) {
      if (client.Send("__getIdentity")) {
        id_reply = client.Recv(/*timeout_ms=*/5000);
      }
      // AU-WSS-IDENT: a REAL capability VALUE (not just identity) round-trips.
      if (client.Send("system/info")) {
        info_reply = client.Recv(/*timeout_ms=*/5000);
      }
      client.Close();
    }
    done.store(true);
  });

  while (!done.load()) {
    base::RunLoop loop;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, loop.QuitClosure(), base::Milliseconds(5));
    loop.Run();
  }
  client_thread.join();
  peer.Stop();

  EXPECT_EQ(id_reply, "legion://chrome/")
      << "remote peer did not reach the Chromium domain root over WSS";
  // The live SystemInfoHandle reports THIS browser process's pid — a verifiably
  // real value, not a canned identity string.
  EXPECT_NE(info_reply.find("\"browserPid\":"), std::string::npos)
      << "system/info did not round-trip a real value over WSS: " << info_reply;
  EXPECT_NE(info_reply.find(std::to_string(base::Process::Current().Pid())),
            std::string::npos)
      << "WSS system/info pid did not match this browser process: " << info_reply;
}

}  // namespace aurelian
