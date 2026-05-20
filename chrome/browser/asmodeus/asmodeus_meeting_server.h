// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ASMODEUS_ASMODEUS_MEETING_SERVER_H_
#define CHROME_BROWSER_ASMODEUS_ASMODEUS_MEETING_SERVER_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/synchronization/waitable_event.h"
#include "base/threading/thread.h"
#include "net/server/http_server.h"
#include "net/server/http_server_request_info.h"

namespace asmodeus {

// Embedded WebSocket signaling server + HTTP server for Asmodeus meetings.
//
// Provides:
// - HTTP: serves meeting.html at /meeting.html
// - WebSocket: relays signaling messages between peers (offers, answers,
//   ICE candidates) for WebRTC peer connection establishment.
//
// Each connected WebSocket client gets a unique peer ID. The server
// broadcasts peer-joined/peer-left events and relays directed messages.
//
// Usage:
//   auto server = std::make_unique<AsmodeusMeetingServer>();
//   server->Start("My Meeting", meeting_html_path);
//   int port = server->port();
//   // Clients connect to ws://localhost:{port} for signaling
//   // and http://localhost:{port}/meeting.html for the UI
class AsmodeusMeetingServer : public net::HttpServer::Delegate {
 public:
  AsmodeusMeetingServer();
  ~AsmodeusMeetingServer() override;

  AsmodeusMeetingServer(const AsmodeusMeetingServer&) = delete;
  AsmodeusMeetingServer& operator=(const AsmodeusMeetingServer&) = delete;

  // Start the server. |meeting_html_content| is the HTML content to serve.
  // Port is auto-selected. Returns true on success.
  bool Start(const std::string& meeting_name,
             const std::string& meeting_html_content);

  // Stop the server and disconnect all clients.
  void Stop();

  bool is_running() const { return server_ != nullptr; }
  int port() const { return port_; }
  const std::string& meeting_name() const { return meeting_name_; }

  // Get the meeting page URL.
  std::string GetMeetingUrl() const;

  // Get the signaling WebSocket URL.
  std::string GetSignalingUrl() const;

  // Get the number of connected signaling peers.
  int GetPeerCount() const;

  // net::HttpServer::Delegate:
  void OnConnect(int connection_id) override;
  void OnHttpRequest(int connection_id,
                     const net::HttpServerRequestInfo& info) override;
  void OnWebSocketRequest(int connection_id,
                          const net::HttpServerRequestInfo& info) override;
  void OnWebSocketMessage(int connection_id, std::string data) override;
  void OnClose(int connection_id) override;

 public:
  // IO thread entry points (called via PostTask from UI thread).
  bool StartOnIOThread(const std::string& meeting_name,
                        const std::string& meeting_html_content);

 private:
  void StartOnIOThread(bool* out_success, base::WaitableEvent* event);
  void StopOnIOThread(base::WaitableEvent* event);

  // Send a JSON message to a specific WebSocket connection.
  void SendJson(int connection_id, const std::string& json);

  // Broadcast a JSON message to all peers except |exclude_id|.
  void BroadcastJson(const std::string& json, int exclude_id);

  // Dedicated IO thread for the server (net::HttpServer needs IO pump).
  std::unique_ptr<base::Thread> io_thread_;
  std::unique_ptr<net::HttpServer> server_;
  std::string meeting_name_;
  std::string meeting_html_content_;
  int port_ = 0;
  int next_peer_id_ = 0;

  // connection_id → peer_id string
  std::map<int, std::string> peers_;
  // peer_id → connection_id (reverse map)
  std::map<std::string, int> peer_connections_;
  // peer_id → display name
  std::map<std::string, std::string> peer_names_;
};

}  // namespace asmodeus

#endif  // CHROME_BROWSER_ASMODEUS_ASMODEUS_MEETING_SERVER_H_
