// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/asmodeus/native_agent/signaling_client.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <random>

#include "base/compiler_specific.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/threading/platform_thread.h"

namespace asmodeus {

SignalingClient::SignalingClient() = default;

SignalingClient::~SignalingClient() {
  Disconnect();
}

bool SignalingClient::Connect(const std::string& host, int port,
                               const std::string& agent_name) {
  agent_name_ = agent_name;

  // Resolve host
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
    struct hostent* he = gethostbyname(host.c_str());
    if (!he) { LOG(ERROR) << "Cannot resolve: " << host; return false; }
    memcpy(&addr.sin_addr, he->h_addr, he->h_length);
  }

  // Connect TCP with retry
  for (int attempt = 0; attempt < 30; attempt++) {
    socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd_ < 0) { LOG(ERROR) << "socket() failed"; return false; }

    if (connect(socket_fd_, reinterpret_cast<struct sockaddr*>(&addr),
                sizeof(addr)) == 0) {
      break;  // Connected
    }

    close(socket_fd_);
    socket_fd_ = -1;

    if (attempt < 29) {
      LOG(INFO) << "Signaling connect retry " << (attempt + 1)
                << "/30 to " << host << ":" << port;
      usleep(1000000);  // 1 second
    } else {
      LOG(ERROR) << "connect() failed after 30 retries to "
                 << host << ":" << port;
      return false;
    }
  }

  // WebSocket HTTP upgrade
  if (!PerformHttpUpgrade(host, port)) {
    close(socket_fd_);
    socket_fd_ = -1;
    return false;
  }

  connected_ = true;

  // Start read loop on IO thread
  io_thread_ = std::make_unique<base::Thread>("SignalingIO");
  io_thread_->Start();
  io_thread_->task_runner()->PostTask(
      FROM_HERE, base::BindOnce(&SignalingClient::ReadLoop,
                                 base::Unretained(this)));

  // Send set-name
  Send("{\"type\":\"set-name\",\"name\":\"" + agent_name_ + "\"}");

  LOG(INFO) << "Signaling connected to " << host << ":" << port;
  return true;
}

void SignalingClient::Disconnect() {
  connected_ = false;
  if (socket_fd_ >= 0) {
    shutdown(socket_fd_, SHUT_RDWR);
    close(socket_fd_);
    socket_fd_ = -1;
  }
  if (io_thread_) {
    io_thread_->Stop();
    io_thread_.reset();
  }
}

void SignalingClient::Send(const std::string& json) {
  if (!connected_ || socket_fd_ < 0) return;
  std::lock_guard<std::mutex> lock(send_mutex_);
  SendWebSocketFrame(json);
}

bool SignalingClient::PerformHttpUpgrade(const std::string& host, int port) {
  // Generate random WebSocket key
  std::random_device rd;
  std::mt19937 gen(rd());
  char key_bytes[16];
  for (int i = 0; i < 16; i++) key_bytes[i] = gen() & 0xFF;
  // Simple base64 of 16 bytes (just use a fixed key for simplicity)
  std::string ws_key = "dGhlIHNhbXBsZSBub25jZQ==";

  std::string request =
      "GET / HTTP/1.1\r\n"
      "Host: " + host + ":" + base::NumberToString(port) + "\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Key: " + ws_key + "\r\n"
      "Sec-WebSocket-Version: 13\r\n"
      "\r\n";

  ssize_t sent = write(socket_fd_, request.c_str(), request.size());
  if (sent <= 0) return false;

  // Read HTTP response headers byte-by-byte until \r\n\r\n
  // Must not over-read into the WebSocket frame that follows
  std::string response;
  char c;
  while (read(socket_fd_, &c, 1) == 1) {
    response += c;
    if (response.size() >= 4 &&
        response.substr(response.size() - 4) == "\r\n\r\n") {
      break;
    }
    if (response.size() > 4096) break;  // safety limit
  }
  return response.find("101") != std::string::npos;
}

void SignalingClient::SendWebSocketFrame(const std::string& payload) {
  // Build masked WebSocket text frame
  uint8_t header[10];
  size_t header_len = 2;
  header[0] = 0x81;  // FIN + text opcode

  if (payload.size() < 126) {
    header[1] = 0x80 | static_cast<uint8_t>(payload.size());  // MASK + length
  } else if (payload.size() < 65536) {
    header[1] = 0x80 | 126;
    header[2] = (payload.size() >> 8) & 0xFF;
    header[3] = payload.size() & 0xFF;
    header_len = 4;
  } else {
    // Large frames not needed for signaling
    return;
  }

  // Masking key (client must mask)
  uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};

  // Build full frame
  std::vector<uint8_t> frame(header_len + 4 + payload.size());
  memcpy(frame.data(), header, header_len);
  // SAFETY: header_len is 2 or 4, payload fits in frame allocation.
  {
    uint8_t* dst = frame.data();
    UNSAFE_BUFFERS(memcpy(dst + header_len, mask, 4));
    for (size_t i = 0; i < payload.size(); i++) {
      size_t offset = header_len + 4 + i;
      UNSAFE_BUFFERS(dst[offset] =
          static_cast<uint8_t>(payload[i]) ^ mask[i % 4]);
    }
  }

  write(socket_fd_, frame.data(), frame.size());
}

std::string SignalingClient::ReadWebSocketFrame() {
  // Read frame header
  uint8_t h[2];
  ssize_t n = read(socket_fd_, h, 2);
  if (n != 2) return "";

  bool masked = (h[1] & 0x80) != 0;
  size_t payload_len = h[1] & 0x7F;

  if (payload_len == 126) {
    uint8_t ext[2];
    if (read(socket_fd_, ext, 2) != 2) return "";
    payload_len = (ext[0] << 8) | ext[1];
  } else if (payload_len == 127) {
    uint8_t ext[8];
    if (read(socket_fd_, ext, 8) != 8) return "";
    payload_len = 0;
    for (int i = 0; i < 8; i++)
      payload_len = (payload_len << 8) | UNSAFE_BUFFERS(ext[i]);
  }

  uint8_t mask[4] = {0};
  if (masked) {
    if (read(socket_fd_, mask, 4) != 4) return "";
  }

  // Read payload
  std::string payload(payload_len, '\0');
  size_t total = 0;
  while (total < payload_len) {
    n = read(socket_fd_, &payload[total], payload_len - total);
    if (n <= 0) return "";
    total += n;
  }

  if (masked) {
    for (size_t i = 0; i < payload.size(); i++)
      payload[i] ^= mask[i % 4];
  }

  return payload;
}

void SignalingClient::ReadLoop() {
  while (connected_) {
    std::string frame = ReadWebSocketFrame();
    if (frame.empty()) {
      if (connected_) {
        LOG(WARNING) << "Signaling connection lost";
        connected_ = false;
      }
      break;
    }
    HandleMessage(frame);
  }
}

void SignalingClient::HandleMessage(const std::string& json) {
  auto parsed = base::JSONReader::Read(json, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) return;

  auto& msg = parsed->GetDict();
  const std::string* type = msg.FindString("type");
  if (!type) return;

  if (*type == "welcome") {
    const std::string* id = msg.FindString("id");
    if (id) my_id_ = *id;

    std::vector<std::string> peers;
    const auto* peer_list = msg.FindList("peers");
    if (peer_list) {
      for (const auto& v : *peer_list) {
        if (v.is_string()) peers.push_back(v.GetString());
      }
    }

    LOG(INFO) << "Signaling: welcome, id=" << my_id_
              << ", peers=" << peers.size();
    if (on_welcome) on_welcome(my_id_, std::move(peers));
  } else if (*type == "peer-joined") {
    const std::string* id = msg.FindString("id");
    if (id && on_peer_joined) on_peer_joined(*id);
  } else if (*type == "peer-left") {
    const std::string* id = msg.FindString("id");
    if (id && on_peer_left) on_peer_left(*id);
  } else if (*type == "peer-name") {
    const std::string* id = msg.FindString("id");
    const std::string* name = msg.FindString("name");
    if (id && name && on_peer_name) on_peer_name(*id, *name);
  } else if (*type == "offer") {
    const std::string* from = msg.FindString("from");
    if (from && on_offer) on_offer(*from, json);
  } else if (*type == "answer") {
    const std::string* from = msg.FindString("from");
    if (from && on_answer) on_answer(*from, json);
  } else if (*type == "ice-candidate") {
    const std::string* from = msg.FindString("from");
    if (from && on_ice_candidate) on_ice_candidate(*from, json);
  }
}

}  // namespace asmodeus
