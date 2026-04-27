// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/asmodeus/native_agent/control_channel.h"

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/threading/platform_thread.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_errors.h"
#include "net/log/net_log_source.h"
#include "net/socket/tcp_server_socket.h"
#include "net/traffic_annotation/network_traffic_annotation.h"

namespace asmodeus {

namespace {

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("asmodeus_agent_control", R"(
      semantics {
        sender: "Asmodeus Native Agent"
        description: "Control channel for agent commands."
        trigger: "Agent startup."
        data: "JSON command/response messages."
        destination: LOCAL
      }
      policy { cookies_allowed: NO setting: "Agent flag." })");

}  // namespace

ControlChannel::ControlChannel() = default;

ControlChannel::~ControlChannel() {
  Stop();
}

bool ControlChannel::Start(int port, CommandCallback callback) {
  command_callback_ = std::move(callback);

  io_thread_ = std::make_unique<base::Thread>("AgentControlIO");
  base::Thread::Options options(base::MessagePumpType::IO, 0);
  if (!io_thread_->StartWithOptions(std::move(options))) {
    LOG(ERROR) << "Failed to start control IO thread";
    return false;
  }

  std::atomic<int> result{0};
  io_thread_->task_runner()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](ControlChannel* self, int port, std::atomic<int>* result) {
            auto socket = std::make_unique<net::TCPServerSocket>(
                nullptr, net::NetLogSource());
            int rv = socket->ListenWithAddressAndPort("127.0.0.1", port, 10);
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
          base::Unretained(this), port, &result));

  for (int i = 0; i < 1000 && result.load() == 0; ++i) {
    base::PlatformThread::Sleep(base::Milliseconds(1));
  }

  return result.load() == 1;
}

void ControlChannel::Stop() {
  if (server_ && io_thread_) {
    std::atomic<bool> done{false};
    io_thread_->task_runner()->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](ControlChannel* self, std::atomic<bool>* done) {
              self->StopOnIOThread();
              done->store(true);
            },
            base::Unretained(this), &done));
    for (int i = 0; i < 1000 && !done.load(); ++i) {
      base::PlatformThread::Sleep(base::Milliseconds(1));
    }
    io_thread_->Stop();
    io_thread_.reset();
  }
}

void ControlChannel::StopOnIOThread() {
  for (int cid : clients_) {
    server_->Close(cid);
  }
  clients_.clear();
  server_.reset();
}

void ControlChannel::EmitEvent(const std::string& event_json) {
  if (!io_thread_) return;
  io_thread_->task_runner()->PostTask(
      FROM_HERE,
      base::BindOnce(&ControlChannel::EmitOnIOThread,
                     base::Unretained(this), event_json));
}

void ControlChannel::EmitOnIOThread(const std::string& json) {
  for (int cid : clients_) {
    server_->SendOverWebSocket(cid, json, kTrafficAnnotation);
  }
}

void ControlChannel::OnConnect(int connection_id) {}

void ControlChannel::OnHttpRequest(
    int connection_id,
    const net::HttpServerRequestInfo& info) {
  // Health check endpoint
  if (info.path == "/health") {
    server_->Send200(connection_id, "{\"ok\":true}", "application/json",
                     kTrafficAnnotation);
  } else {
    server_->Send404(connection_id, kTrafficAnnotation);
  }
}

void ControlChannel::OnWebSocketRequest(
    int connection_id,
    const net::HttpServerRequestInfo& info) {
  server_->AcceptWebSocket(connection_id, info, kTrafficAnnotation);
  clients_.push_back(connection_id);
  LOG(INFO) << "Control client connected: " << connection_id;
}

void ControlChannel::OnWebSocketMessage(int connection_id, std::string data) {
  // Parse JSON command
  auto parsed = base::JSONReader::Read(data, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    server_->SendOverWebSocket(
        connection_id,
        R"({"ok":false,"error":"Invalid JSON"})",
        kTrafficAnnotation);
    return;
  }

  auto& dict = parsed->GetDict();
  const std::string* command = dict.FindString("command");
  if (!command) {
    server_->SendOverWebSocket(
        connection_id,
        R"({"ok":false,"error":"Missing 'command' field"})",
        kTrafficAnnotation);
    return;
  }

  // Invoke callback — this may block (e.g., speak blocks for TTS duration)
  std::string response;
  if (command_callback_) {
    response = command_callback_(*command, data);
  } else {
    response = R"({"ok":false,"error":"No handler"})";
  }

  server_->SendOverWebSocket(connection_id, response, kTrafficAnnotation);
}

void ControlChannel::OnClose(int connection_id) {
  clients_.erase(
      std::remove(clients_.begin(), clients_.end(), connection_id),
      clients_.end());
  LOG(INFO) << "Control client disconnected: " << connection_id;
}

}  // namespace asmodeus
