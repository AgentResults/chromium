// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C9 — the bring-up WSS peer: a remote caller reaches the Chromium
// domain (legion://chrome/) over a real WebSocket and gets a real reply,
// dispatched through the Velite root handle. Loopback (one machine); the
// cross-host + MCP gateway SIT is sequenced after the Agrippa control-plane.

#include "aurelian/federation/wss_peer.h"

#include "base/threading/thread_restrictions.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "content/public/test/browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace aurelian {

using AurelianWssPeerBrowserTest = InProcessBrowserTest;

IN_PROC_BROWSER_TEST_F(AurelianWssPeerBrowserTest, RemotePeerReachesChromeRoot) {
  // Socket connect/recv block; allow it on the test thread.
  base::ScopedAllowBlockingForTesting allow_blocking;

  // Serve the Chromium domain on an OS-assigned port.
  WssPeer peer;
  uint16_t port = peer.Start(0, &DispatchChromeRoot);
  ASSERT_NE(port, 0u) << "peer failed to bind";

  // A remote caller connects and asks the Chromium domain for its identity.
  WssClient client;
  ASSERT_TRUE(client.Connect("127.0.0.1", port));
  ASSERT_TRUE(client.Send("__getIdentity"));

  std::string reply = client.Recv(/*timeout_ms=*/5000);
  EXPECT_EQ(reply, "legion://chrome/")
      << "remote peer did not reach the Chromium domain root over WSS";

  client.Close();
  peer.Stop();
}

}  // namespace aurelian
