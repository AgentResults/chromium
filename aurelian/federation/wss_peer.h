// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C9: the bring-up external WSS peer (design §13). The browser-process
// AgentSpace runs its OWN WebSocket peer so a remote caller can reach the
// Chromium domain (legion://chrome/...) over the wire before Agrippa's
// federation router exists. This is the transport half; the cross-host + MCP
// gateway SIT is sequenced after the Agrippa control-plane (see the c9-blocked
// note). Velite types are pimpl-hidden so Chrome/test TUs stay Velite-free.

#ifndef AURELIAN_FEDERATION_WSS_PEER_H_
#define AURELIAN_FEDERATION_WSS_PEER_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace aurelian {

// Dispatches a request verb to the Chromium-domain root handle
// (legion://chrome/) via its Velite ask, returning the serialized reply
// ("broken:<reason>" if the verb is not callable). The production bring-up peer
// wires this as its DispatchFn; exposed here for the bootstrap + tests.
std::string DispatchChromeRoot(const std::string& request);

// Server side: accepts one peer and serves request/reply frames. Each received
// frame is handed to `dispatch` (which routes it into the AgentSpace) and the
// returned string is sent back. Runs its accept/serve loop on a dedicated
// thread.
class WssPeer {
 public:
  using DispatchFn = std::function<std::string(const std::string& request)>;

  WssPeer();
  ~WssPeer();

  WssPeer(const WssPeer&) = delete;
  WssPeer& operator=(const WssPeer&) = delete;

  // Binds a listener on `port` (0 = OS-assigned) and starts serving. Returns
  // the actually-bound port, or 0 on failure.
  uint16_t Start(uint16_t port, DispatchFn dispatch);

  // Stops the serve loop and joins the thread.
  void Stop();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Client side (used by tests / a remote caller): a synchronous WebSocket client.
class WssClient {
 public:
  WssClient();
  ~WssClient();

  WssClient(const WssClient&) = delete;
  WssClient& operator=(const WssClient&) = delete;

  // Connects to ws://host:port/ . Returns false on failure.
  bool Connect(const std::string& host, uint16_t port);
  bool Send(const std::string& message);
  // Polls for a reply up to `timeout_ms`. Returns empty on timeout/closed.
  std::string Recv(int timeout_ms);
  void Close();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace aurelian

#endif  // AURELIAN_FEDERATION_WSS_PEER_H_
