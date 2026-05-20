// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ASMODEUS_CONTROL_CLIENT_H_
#define CHROME_BROWSER_ASMODEUS_CONTROL_CLIENT_H_

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "base/threading/thread.h"

namespace asmodeus {

// WebSocket client that connects to an agent's control channel.
// Used by the meeting coordinator to send commands and receive events.
class ControlClient {
 public:
  ControlClient();
  ~ControlClient();

  ControlClient(const ControlClient&) = delete;
  ControlClient& operator=(const ControlClient&) = delete;

  // Connect to agent's control port. Blocks until connected or fails.
  bool Connect(const std::string& host, int port);
  void Disconnect();
  bool is_connected() const { return connected_; }

  // Send a command and wait for the response (blocking).
  // Timeout in milliseconds. Returns empty string on timeout/error.
  std::string SendCommand(const std::string& json, int timeout_ms = 10000);

  // Callback for unsolicited events (fires on IO thread).
  // Coordinator should PostTask to UI thread before accessing state.
  std::function<void(const std::string& event_json)> on_event;

 private:
  bool PerformHttpUpgrade(const std::string& host, int port);
  void SendWebSocketFrame(const std::string& payload);
  std::string ReadWebSocketFrame();
  void ReadLoop();

  std::mutex send_mutex_;
  int socket_fd_ = -1;
  std::atomic<bool> connected_{false};
  std::unique_ptr<base::Thread> io_thread_;

  // For synchronous SendCommand: response is stored here by ReadLoop.
  std::mutex response_mutex_;
  std::condition_variable response_cv_;
  std::string pending_response_;
  bool response_ready_ = false;
};

}  // namespace asmodeus

#endif  // CHROME_BROWSER_ASMODEUS_CONTROL_CLIENT_H_
