// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/asmodeus/control_client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"

namespace asmodeus {

ControlClient::ControlClient() = default;

ControlClient::~ControlClient() {
  Disconnect();
}

bool ControlClient::Connect(const std::string& host, int port) {
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

  // Retry connection (agent may still be starting)
  for (int attempt = 0; attempt < 30; attempt++) {
    socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd_ < 0) return false;
    if (connect(socket_fd_, reinterpret_cast<struct sockaddr*>(&addr),
                sizeof(addr)) == 0) {
      break;
    }
    close(socket_fd_);
    socket_fd_ = -1;
    if (attempt < 29) usleep(1000000);
    else { LOG(ERROR) << "ControlClient: connect failed to " << host << ":" << port; return false; }
  }

  if (!PerformHttpUpgrade(host, port)) {
    close(socket_fd_); socket_fd_ = -1; return false;
  }

  connected_ = true;

  // Start read loop
  io_thread_ = std::make_unique<base::Thread>("ControlClientIO");
  io_thread_->Start();
  io_thread_->task_runner()->PostTask(
      FROM_HERE, base::BindOnce(&ControlClient::ReadLoop, base::Unretained(this)));

  LOG(INFO) << "ControlClient connected to " << host << ":" << port;
  return true;
}

void ControlClient::Disconnect() {
  connected_ = false;
  if (socket_fd_ >= 0) {
    // Close socket — this will cause read() to fail in ReadLoop,
    // which then exits. Any waiting SendCommand will see connected_==false
    // and return empty. No mutex/cv needed here — avoids DCHECK on UI thread.
    int fd = socket_fd_;
    socket_fd_ = -1;
    shutdown(fd, SHUT_RDWR);
    close(fd);
  }
  // Don't touch response_mutex_ or response_cv_ here — we may be on the
  // UI thread where base sync primitives are forbidden. The ReadLoop will
  // exit on its own when read() fails on the closed socket.
  if (io_thread_) {
    io_thread_.release();  // Release ownership, let it clean up itself
  }
}

std::string ControlClient::SendCommand(const std::string& json, int timeout_ms) {
  if (!connected_) return "";

  {
    std::lock_guard<std::mutex> lock(response_mutex_);
    response_ready_ = false;
    pending_response_.clear();
  }

  {
    std::lock_guard<std::mutex> lock(send_mutex_);
    SendWebSocketFrame(json);
  }

  // Wait for response
  std::unique_lock<std::mutex> lock(response_mutex_);
  if (response_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                             [this] { return response_ready_; })) {
    return pending_response_;
  }
  return "";  // timeout
}

bool ControlClient::PerformHttpUpgrade(const std::string& host, int port) {
  std::string request =
      "GET / HTTP/1.1\r\n"
      "Host: " + host + ":" + base::NumberToString(port) + "\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "Sec-WebSocket-Version: 13\r\n"
      "\r\n";

  write(socket_fd_, request.c_str(), request.size());

  // Read headers byte-by-byte to avoid consuming WebSocket frames
  std::string response;
  char c;
  while (read(socket_fd_, &c, 1) == 1) {
    response += c;
    if (response.size() >= 4 &&
        response.substr(response.size() - 4) == "\r\n\r\n") break;
    if (response.size() > 4096) break;
  }
  return response.find("101") != std::string::npos;
}

void ControlClient::SendWebSocketFrame(const std::string& payload) {
  uint8_t header[4];
  size_t header_len = 2;
  header[0] = 0x81;
  if (payload.size() < 126) {
    header[1] = 0x80 | static_cast<uint8_t>(payload.size());
  } else {
    header[1] = 0x80 | 126;
    header[2] = (payload.size() >> 8) & 0xFF;
    header[3] = payload.size() & 0xFF;
    header_len = 4;
  }

  uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
  std::vector<uint8_t> frame(header_len + 4 + payload.size());
  memcpy(frame.data(), header, header_len);
  memcpy(frame.data() + header_len, mask, 4);
  for (size_t i = 0; i < payload.size(); i++) {
    frame[header_len + 4 + i] = static_cast<uint8_t>(payload[i]) ^ mask[i % 4];
  }
  write(socket_fd_, frame.data(), frame.size());
}

std::string ControlClient::ReadWebSocketFrame() {
  uint8_t h[2];
  if (read(socket_fd_, h, 2) != 2) return "";

  bool masked = (h[1] & 0x80) != 0;
  size_t len = h[1] & 0x7F;
  if (len == 126) {
    uint8_t ext[2];
    if (read(socket_fd_, ext, 2) != 2) return "";
    len = (ext[0] << 8) | ext[1];
  }

  uint8_t mask[4] = {0};
  if (masked) {
    if (read(socket_fd_, mask, 4) != 4) return "";
  }

  std::string payload(len, '\0');
  size_t total = 0;
  while (total < len) {
    ssize_t n = read(socket_fd_, &payload[total], len - total);
    if (n <= 0) return "";
    total += n;
  }
  if (masked) {
    for (size_t i = 0; i < payload.size(); i++)
      payload[i] ^= mask[i % 4];
  }
  return payload;
}

void ControlClient::ReadLoop() {
  while (connected_) {
    std::string frame = ReadWebSocketFrame();
    if (frame.empty()) {
      if (connected_) { connected_ = false; LOG(WARNING) << "Control connection lost"; }
      break;
    }

    // Determine if this is a response (has "id") or an event (has "event")
    auto parsed = base::JSONReader::Read(frame, base::JSON_PARSE_RFC);
    if (parsed && parsed->is_dict()) {
      auto& dict = parsed->GetDict();
      if (dict.Find("id")) {
        // Response to a command — wake up SendCommand
        std::lock_guard<std::mutex> lock(response_mutex_);
        pending_response_ = frame;
        response_ready_ = true;
        response_cv_.notify_all();
      } else if (dict.FindString("event")) {
        // Unsolicited event — dispatch callback
        if (on_event) on_event(frame);
      }
    }
  }
}

}  // namespace asmodeus
