// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/federation/wss_peer.h"

#include <atomic>
#include <cstdint>
#include <thread>

#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "velite/agentspaces-wire/handle.hpp"
#include "velite/agentspaces-wire/value_handle.hpp"
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

// The Chromium-domain root handle (legion://chrome/) — the federation entry
// point a remote peer reaches. Mirrors the bootstrap root.
class ChromeRootHandle : public velite::agentspaces::Handle {
 public:
  velite::agentspaces::StateKind state_kind() const override {
    return velite::agentspaces::StateKind::ResolvedValue;
  }
  const velite::agentspaces::Value& resolved_value() const override {
    return identity_;
  }
  std::shared_ptr<velite::agentspaces::Handle> resolved_handle()
      const override {
    return nullptr;
  }
  std::string_view broken_reason() const override { return ""; }
  std::string sturdy_identity() const override { return "legion://chrome/"; }

  std::shared_ptr<velite::agentspaces::Handle> ask_impl(
      std::string_view msg,
      const velite::agentspaces::Value& /*spec*/) override {
    using V = velite::agentspaces::Value;
    using VH = velite::agentspaces::ValueHandle;
    if (msg == "__getIdentity") {
      return VH::make(V(std::string("legion://chrome/")));
    }
    if (msg == "describe") {
      return VH::make(V::make_object({
          {"name", V(std::string("chrome"))},
          {"uri", V(std::string("legion://chrome/"))},
          {"embodiment", V(std::string("aurelian"))},
      }));
    }
    return VH::make_broken("not-callable");
  }

  void tell(std::string_view /*msg*/,
            const velite::agentspaces::Value& /*data*/) override {}

 private:
  velite::agentspaces::Value identity_{std::string("legion://chrome/")};
};

}  // namespace

std::string DispatchChromeRoot(const std::string& request) {
  static ChromeRootHandle* root = new ChromeRootHandle();
  std::shared_ptr<velite::agentspaces::Handle> result =
      root->ask(request, velite::agentspaces::Value());
  if (!result) {
    return "broken:null";
  }
  if (result->state_kind() == velite::agentspaces::StateKind::Broken) {
    return std::string("broken:") + std::string(result->broken_reason());
  }
  const velite::agentspaces::Value& v = result->resolved_value();
  if (v.is_string()) {
    return v.as_string();
  }
  if (v.is_object()) {
    // Compact: report the identity field for an object reply.
    const auto& o = v.as_object();
    if (auto it = o.find("uri"); it != o.end() && it->second.is_string()) {
      return it->second.as_string();
    }
  }
  return "ok";
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
    velite::WebSocketChannel conn;
    // Accept one peer (non-blocking accept; poll until a peer arrives).
    while (!stop.load()) {
      if (listener.accept(conn, velite::StringView("wss-peer"))) {
        break;
      }
      SleepBriefly();
    }
    if (stop.load()) {
      return;
    }
    // Serve request/reply frames until the peer closes.
    uint8_t buf[kRxBuffer];
    while (!stop.load() && conn.is_open()) {
      size_t len = 0;
      velite::ChannelError err = conn.recv(buf, sizeof(buf), &len);
      if (err == velite::ChannelError::OK && len > 0) {
        std::string request(reinterpret_cast<char*>(buf), len);
        std::string reply = dispatch ? dispatch(request) : std::string();
        conn.send(reinterpret_cast<const uint8_t*>(reply.data()),
                  reply.size());
      } else if (err == velite::ChannelError::Closed) {
        break;
      } else {
        SleepBriefly();
      }
    }
    conn.close();
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
