// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ASMODEUS_NATIVE_AGENT_CONTROL_CHANNEL_H_
#define CHROME_BROWSER_ASMODEUS_NATIVE_AGENT_CONTROL_CHANNEL_H_

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "base/threading/thread.h"
#include "net/server/http_server.h"
#include "net/server/http_server_request_info.h"

namespace asmodeus {

// WebSocket command server for controlling the native agent.
// Runs on a dedicated IO thread. Commands are dispatched via callbacks.
class ControlChannel : public net::HttpServer::Delegate {
 public:
  // Callback invoked on the IO thread when a command arrives.
  // Returns the JSON response string.
  using CommandCallback = std::function<std::string(const std::string& command,
                                                     const std::string& params_json)>;

  ControlChannel();
  ~ControlChannel() override;

  ControlChannel(const ControlChannel&) = delete;
  ControlChannel& operator=(const ControlChannel&) = delete;

  // Start the WebSocket server on the given port.
  bool Start(int port, CommandCallback callback);
  void Stop();

  // Send an unsolicited event to all connected clients.
  void EmitEvent(const std::string& event_json);

  int port() const { return port_; }
  bool is_running() const { return server_ != nullptr; }

  // net::HttpServer::Delegate
  void OnConnect(int connection_id) override;
  void OnHttpRequest(int connection_id,
                     const net::HttpServerRequestInfo& info) override;
  void OnWebSocketRequest(int connection_id,
                          const net::HttpServerRequestInfo& info) override;
  void OnWebSocketMessage(int connection_id, std::string data) override;
  void OnClose(int connection_id) override;

 private:
  void StartOnIOThread(int port, bool* out_success);
  void StopOnIOThread();
  void EmitOnIOThread(const std::string& json);

  std::unique_ptr<base::Thread> io_thread_;
  std::unique_ptr<net::HttpServer> server_;
  CommandCallback command_callback_;
  std::vector<int> clients_;  // connected WebSocket client IDs
  int port_ = 0;
};

}  // namespace asmodeus

#endif  // CHROME_BROWSER_ASMODEUS_NATIVE_AGENT_CONTROL_CHANNEL_H_
