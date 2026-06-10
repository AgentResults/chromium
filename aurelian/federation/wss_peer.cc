// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/federation/wss_peer.h"

#include <atomic>
#include <cstdint>
#include <thread>

#include "aurelian/bootstrap/browser_main_extra.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "velite/channel.hpp"
#include "velite/string_view.hpp"
#include "velite/ws_channel.hpp"
#include "velite/ws_listener.hpp"

namespace aurelian {

namespace {

constexpr size_t kRxBuffer = 65536;

void SleepBriefly() {
  base::PlatformThread::Sleep(base::Milliseconds(2));
}

}  // namespace

std::string DispatchChromeRoot(const std::string& request) {
  // ACM-2w (design section 5): this scaffold consumes the ONE
  // ChromeDispatchFn exported over the INSTALLED sealed root — its former
  // lazy `static ChromeRoot*` was a second consumption of ambient authority
  // (the counted-construction audit pins constructions at 1). `request` is
  // a slash-path ("__getIdentity", "system/info"); the WSS wire carries no
  // spec. The serve-thread hop is the HS-1 completion-signaled bridge
  // inside the exported fn.
  return InstalledChromeDispatch()(request, std::string());
}

// ---------------------------------------------------------------------------
// WssPeer
// ---------------------------------------------------------------------------

struct WssPeer::Impl {
  velite::WebSocketListener listener;
  std::thread serve_thread;
  std::atomic<bool> stop{false};
  DispatchFn dispatch;

  void Serve() {
    // HEAP-allocated: the velite WS channel embeds its MTU-sized rx buffer
    // inline (WS_CHANNEL_RX_BUFFER_BYTES is 1 MiB + 4 KiB since the
    // upstream frame-reassembly fix, Legion 63230cc169) — as a stack local
    // it overflows this std::thread's 512 KiB stack at function entry.
    auto conn = std::make_unique<velite::WebSocketChannel>();
    // Accept one peer (non-blocking accept; poll until a peer arrives).
    while (!stop.load()) {
      if (listener.accept(*conn, velite::StringView("wss-peer"))) {
        break;
      }
      SleepBriefly();
    }
    if (stop.load()) {
      return;
    }
    // Serve request/reply frames until the peer closes.
    auto buf = std::make_unique<uint8_t[]>(kRxBuffer);
    while (!stop.load() && conn->is_open()) {
      size_t len = 0;
      velite::ChannelError err = conn->recv(buf.get(), kRxBuffer, &len);
      if (err == velite::ChannelError::OK && len > 0) {
        std::string request(reinterpret_cast<char*>(buf.get()), len);
        std::string reply = dispatch ? dispatch(request) : std::string();
        conn->send(reinterpret_cast<const uint8_t*>(reply.data()),
                   reply.size());
      } else if (err == velite::ChannelError::Closed) {
        break;
      } else {
        SleepBriefly();
      }
    }
    conn->close();
  }
};

WssPeer::WssPeer() : impl_(std::make_unique<Impl>()) {}

WssPeer::~WssPeer() {
  Stop();
}

uint16_t WssPeer::Start(uint16_t port, DispatchFn dispatch) {
  impl_->dispatch = std::move(dispatch);
  if (!impl_->listener.listen(port)) {
    return 0;
  }
  uint16_t bound = impl_->listener.port();
  Impl* impl = impl_.get();
  impl_->serve_thread = std::thread([impl]() { impl->Serve(); });
  return bound;
}

void WssPeer::Stop() {
  if (!impl_) {
    return;
  }
  impl_->stop.store(true);
  if (impl_->serve_thread.joinable()) {
    impl_->serve_thread.join();
  }
  impl_->listener.close();
}

// ---------------------------------------------------------------------------
// WssClient
// ---------------------------------------------------------------------------

struct WssClient::Impl {
  velite::WebSocketChannel channel;
};

WssClient::WssClient() : impl_(std::make_unique<Impl>()) {}

WssClient::~WssClient() {
  Close();
}

bool WssClient::Connect(const std::string& host, uint16_t port) {
  return impl_->channel.connect_handshake(host.c_str(), port, "/",
                                          velite::StringView("wss-client"));
}

bool WssClient::Send(const std::string& message) {
  return impl_->channel.send(reinterpret_cast<const uint8_t*>(message.data()),
                             message.size()) == velite::ChannelError::OK;
}

std::string WssClient::Recv(int timeout_ms) {
  uint8_t buf[kRxBuffer];
  int elapsed = 0;
  while (elapsed < timeout_ms) {
    size_t len = 0;
    velite::ChannelError err = impl_->channel.recv(buf, sizeof(buf), &len);
    if (err == velite::ChannelError::OK && len > 0) {
      return std::string(reinterpret_cast<char*>(buf), len);
    }
    if (err == velite::ChannelError::Closed) {
      return std::string();
    }
    SleepBriefly();
    elapsed += 2;
  }
  return std::string();
}

void WssClient::Close() {
  if (impl_) {
    impl_->channel.close();
  }
}

}  // namespace aurelian
