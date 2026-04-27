// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ASMODEUS_NATIVE_AGENT_SIGNALING_CLIENT_H_
#define CHROME_BROWSER_ASMODEUS_NATIVE_AGENT_SIGNALING_CLIENT_H_

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "base/threading/thread.h"

namespace asmodeus {

// Minimal WebSocket client for connecting to AsmodeusMeetingServer signaling.
// Runs on a dedicated IO thread. Messages are dispatched via callbacks.
class SignalingClient {
 public:
  using OnWelcome = std::function<void(const std::string& my_id,
                                        std::vector<std::string> peers)>;
  using OnPeerJoined = std::function<void(const std::string& peer_id)>;
  using OnPeerLeft = std::function<void(const std::string& peer_id)>;
  using OnOffer = std::function<void(const std::string& from,
                                      const std::string& sdp_json)>;
  using OnAnswer = std::function<void(const std::string& from,
                                       const std::string& sdp_json)>;
  using OnIceCandidate = std::function<void(const std::string& from,
                                             const std::string& candidate_json)>;
  using OnPeerName = std::function<void(const std::string& peer_id,
                                         const std::string& name)>;

  SignalingClient();
  ~SignalingClient();

  SignalingClient(const SignalingClient&) = delete;
  SignalingClient& operator=(const SignalingClient&) = delete;

  // Connect to ws://host:port. Blocks until connected or fails.
  bool Connect(const std::string& host, int port,
               const std::string& agent_name);
  void Disconnect();
  bool is_connected() const { return connected_; }
  const std::string& my_id() const { return my_id_; }

  // Send signaling messages
  void Send(const std::string& json);

  // Callbacks — called on the IO thread
  OnWelcome on_welcome;
  OnPeerJoined on_peer_joined;
  OnPeerLeft on_peer_left;
  OnOffer on_offer;
  OnAnswer on_answer;
  OnIceCandidate on_ice_candidate;
  OnPeerName on_peer_name;

 private:
  void ReadLoop();
  bool PerformHttpUpgrade(const std::string& host, int port);
  void SendWebSocketFrame(const std::string& payload);
  std::string ReadWebSocketFrame();
  void HandleMessage(const std::string& json);

  std::mutex send_mutex_;  // protects socket writes
  int socket_fd_ = -1;
  std::string my_id_;
  std::string agent_name_;
  std::atomic<bool> connected_{false};
  std::unique_ptr<base::Thread> io_thread_;
};

}  // namespace asmodeus

#endif  // CHROME_BROWSER_ASMODEUS_NATIVE_AGENT_SIGNALING_CLIENT_H_
