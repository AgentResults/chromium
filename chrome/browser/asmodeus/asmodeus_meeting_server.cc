// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/asmodeus/asmodeus_meeting_server.h"

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/run_loop.h"
#include "base/json/json_writer.h"
#include "base/threading/thread_restrictions.h"
#include "base/logging.h"
#include "base/threading/platform_thread.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/values.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_errors.h"
#include "net/log/net_log_source.h"
#include "net/socket/tcp_server_socket.h"
#include "net/traffic_annotation/network_traffic_annotation.h"

namespace asmodeus {

namespace {

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("asmodeus_meeting_server", R"(
      semantics {
        sender: "Asmodeus Meeting Server"
        description: "WebRTC signaling and meeting page server for Asmodeus."
        trigger: "CDP Asmodeus.createMeeting command."
        data: "WebRTC signaling messages (SDP offers/answers, ICE candidates)."
        destination: LOCAL
      }
      policy {
        cookies_allowed: NO
        setting: "Controlled via CDP."
      })");

std::string MakeJson(const base::DictValue& dict) {
  std::string json;
  base::JSONWriter::Write(dict, &json);
  return json;
}

}  // namespace

AsmodeusMeetingServer::AsmodeusMeetingServer() = default;

AsmodeusMeetingServer::~AsmodeusMeetingServer() {
  Stop();
}

bool AsmodeusMeetingServer::Start(const std::string& meeting_name,
                                   const std::string& meeting_html_content) {
  meeting_name_ = meeting_name;
  meeting_html_content_ = meeting_html_content;

  // Create a dedicated IO thread for the server.
  io_thread_ = std::make_unique<base::Thread>("AsmodeusMeetingIO");
  base::Thread::Options options(base::MessagePumpType::IO, 0);
  if (!io_thread_->StartWithOptions(std::move(options))) {
    LOG(ERROR) << "Failed to start meeting IO thread";
    return false;
  }

  // Post server creation to the IO thread. Use a simple atomic flag
  // and spin-wait briefly (socket bind is < 1ms).
  std::atomic<int> result{0}; // 0=pending, 1=success, -1=fail
  io_thread_->task_runner()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](AsmodeusMeetingServer* self, std::atomic<int>* result) {
            auto socket = std::make_unique<net::TCPServerSocket>(
                nullptr, net::NetLogSource());
            int rv = socket->ListenWithAddressAndPort("127.0.0.1", 0, 10);
            if (rv != net::OK) {
              result->store(-1);
              return;
            }
            net::IPEndPoint endpoint;
            socket->GetLocalAddress(&endpoint);
            self->port_ = endpoint.port();
            self->server_ = std::make_unique<net::HttpServer>(
                std::move(socket), self);
            result->store(1);
          },
          base::Unretained(this), &result));

  // Spin-wait for the server to start (typically < 1ms).
  for (int i = 0; i < 1000 && result.load() == 0; ++i) {
    base::PlatformThread::Sleep(base::Milliseconds(1));
  }

  bool success = result.load() == 1;
  if (success) {
    LOG(INFO) << "AsmodeusMeetingServer started on port " << port_
              << " for meeting: " << meeting_name_;
  }
  return success;
}

bool AsmodeusMeetingServer::StartOnIOThread(
    const std::string& meeting_name,
    const std::string& meeting_html_content) {
  meeting_name_ = meeting_name;
  meeting_html_content_ = meeting_html_content;

  auto socket = std::make_unique<net::TCPServerSocket>(nullptr, net::NetLogSource());
  int rv = socket->ListenWithAddressAndPort("127.0.0.1", 0, 10);
  if (rv != net::OK) {
    LOG(ERROR) << "Failed to listen: " << net::ErrorToString(rv);
    return false;
  }

  net::IPEndPoint endpoint;
  socket->GetLocalAddress(&endpoint);
  port_ = endpoint.port();

  server_ = std::make_unique<net::HttpServer>(std::move(socket), this);
  LOG(INFO) << "AsmodeusMeetingServer started on port " << port_
            << " for meeting: " << meeting_name_;
  return true;
}

void AsmodeusMeetingServer::StartOnIOThread(bool* out_success,
                                             base::WaitableEvent* event) {
  // Runs on our dedicated AsmodeusMeetingIO thread (not Chrome's IO thread).

  auto socket = std::make_unique<net::TCPServerSocket>(nullptr, net::NetLogSource());
  int rv = socket->ListenWithAddressAndPort("127.0.0.1", 0, 10);
  if (rv != net::OK) {
    LOG(ERROR) << "Failed to listen: " << net::ErrorToString(rv);
    *out_success = false;
    if (event) event->Signal();
    return;
  }

  net::IPEndPoint endpoint;
  socket->GetLocalAddress(&endpoint);
  port_ = endpoint.port();

  server_ = std::make_unique<net::HttpServer>(std::move(socket), this);
  *out_success = true;
  if (event) event->Signal();
}

void AsmodeusMeetingServer::Stop() {
  if (!io_thread_) return;

  // Post cleanup to IO thread — net::HttpServer must be destroyed on
  // its creation thread (weak_ptr sequence check).
  io_thread_->task_runner()->PostTask(
      FROM_HERE,
      base::BindOnce(&AsmodeusMeetingServer::StopOnIOThread,
                     base::Unretained(this),
                     static_cast<base::WaitableEvent*>(nullptr)));
  // Join the IO thread. This uses sync primitives, which requires
  // ScopedAllowBaseSyncPrimitives (AsmodeusMeetingServer is friended).
  {
    base::ScopedAllowBaseSyncPrimitivesOutsideBlockingScope allow_sync;
    io_thread_->Stop();
  }
  io_thread_.reset();
  port_ = 0;
  LOG(INFO) << "AsmodeusMeetingServer stopped.";
}

void AsmodeusMeetingServer::StopOnIOThread(base::WaitableEvent* event) {
  // Copy IDs before iterating — server_->Close() triggers OnClose()
  // synchronously on this thread, which erases from peers_.
  std::vector<int> ids;
  ids.reserve(peers_.size());
  for (const auto& [connection_id, peer_id] : peers_)
    ids.push_back(connection_id);
  peers_.clear();
  peer_connections_.clear();
  peer_names_.clear();
  for (int id : ids)
    server_->Close(id);
  server_.reset();
  if (event) event->Signal();
}

std::string AsmodeusMeetingServer::GetMeetingUrl() const {
  return base::StringPrintf("http://127.0.0.1:%d/meeting.html", port_);
}

std::string AsmodeusMeetingServer::GetSignalingUrl() const {
  return base::StringPrintf("ws://127.0.0.1:%d", port_);
}

int AsmodeusMeetingServer::GetPeerCount() const {
  return static_cast<int>(peers_.size());
}

// ── net::HttpServer::Delegate callbacks (run on IO thread) ──

void AsmodeusMeetingServer::OnConnect(int connection_id) {
}

void AsmodeusMeetingServer::OnHttpRequest(
    int connection_id,
    const net::HttpServerRequestInfo& info) {
  if (info.path.starts_with("/meeting.html") || info.path == "/") {
    server_->Send200(connection_id, meeting_html_content_, "text/html",
                     kTrafficAnnotation);
  } else {
    server_->Send404(connection_id, kTrafficAnnotation);
  }
}

void AsmodeusMeetingServer::OnWebSocketRequest(
    int connection_id,
    const net::HttpServerRequestInfo& info) {
  server_->AcceptWebSocket(connection_id, info, kTrafficAnnotation);

  std::string peer_id = "peer-" + base::NumberToString(++next_peer_id_);
  peers_[connection_id] = peer_id;
  peer_connections_[peer_id] = connection_id;

  // Build list of existing peers.
  base::ListValue peer_list;
  for (const auto& [cid, pid] : peers_) {
    if (cid != connection_id) {
      peer_list.Append(pid);
    }
  }

  // Send welcome.
  base::DictValue welcome;
  welcome.Set("type", "welcome");
  welcome.Set("id", peer_id);
  welcome.Set("peers", std::move(peer_list));
  SendJson(connection_id, MakeJson(welcome));

  // Notify existing peers.
  base::DictValue joined;
  joined.Set("type", "peer-joined");
  joined.Set("id", peer_id);
  BroadcastJson(MakeJson(joined), connection_id);
}

void AsmodeusMeetingServer::OnWebSocketMessage(int connection_id,
                                                std::string data) {
  auto it = peers_.find(connection_id);
  if (it == peers_.end()) return;
  const std::string& sender_id = it->second;

  auto parsed = base::JSONReader::Read(data, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) return;

  auto& msg = parsed->GetDict();
  const std::string* type = msg.FindString("type");
  if (!type) return;

  if (*type == "set-name") {
    const std::string* name = msg.FindString("name");
    if (name) {
      peer_names_[sender_id] = *name;
      base::DictValue name_msg;
      name_msg.Set("type", "peer-name");
      name_msg.Set("id", sender_id);
      name_msg.Set("name", *name);
      BroadcastJson(MakeJson(name_msg), connection_id);
    }
    return;
  }

  const std::string* to = msg.FindString("to");
  if (to) {
    auto target_it = peer_connections_.find(*to);
    if (target_it != peer_connections_.end()) {
      msg.Set("from", sender_id);
      SendJson(target_it->second, MakeJson(msg));
    }
  } else {
    msg.Set("from", sender_id);
    BroadcastJson(MakeJson(msg), connection_id);
  }
}

void AsmodeusMeetingServer::OnClose(int connection_id) {
  auto it = peers_.find(connection_id);
  if (it == peers_.end()) return;

  std::string peer_id = it->second;
  peers_.erase(it);
  peer_connections_.erase(peer_id);
  peer_names_.erase(peer_id);

  base::DictValue left_msg;
  left_msg.Set("type", "peer-left");
  left_msg.Set("id", peer_id);
  BroadcastJson(MakeJson(left_msg), -1);
}

void AsmodeusMeetingServer::SendJson(int connection_id,
                                      const std::string& json) {
  if (server_) {
    server_->SendOverWebSocket(connection_id, json, kTrafficAnnotation);
  }
}

void AsmodeusMeetingServer::BroadcastJson(const std::string& json,
                                           int exclude_id) {
  for (const auto& [cid, pid] : peers_) {
    if (cid != exclude_id) {
      SendJson(cid, json);
    }
  }
}

}  // namespace asmodeus
