// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/federation/uds_register.h"

#include <atomic>
#include <memory>
#include <thread>
#include <utility>

#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "velite/agentspaces-wire/dispatcher.hpp"
#include "velite/agentspaces-wire/handle.hpp"
#include "velite/agentspaces-wire/value_handle.hpp"
#include "velite/channel.hpp"
#include "velite/uds_channel.hpp"

namespace aurelian {

namespace {

using velite::agentspaces::Handle;
using velite::agentspaces::Value;
using velite::agentspaces::ValueHandle;
using velite::agentspaces::wire::Dispatcher;

constexpr size_t kRxBuffer = 65536;
constexpr char kChromeUriPrefix[] = "legion://chrome";

// Map a forwarded dispatchAt{uri,verb} to the slash-path RootDispatch expects:
//   legion://chrome            + __getIdentity -> "__getIdentity"
//   legion://chrome/system     + info          -> "system/info"
std::string DerivePath(const std::string& uri, const std::string& verb) {
  std::string rel;
  if (uri.compare(0, sizeof(kChromeUriPrefix) - 1, kChromeUriPrefix) == 0) {
    rel = uri.substr(sizeof(kChromeUriPrefix) - 1);
  } else {
    rel = uri;  // already-relative form
  }
  while (!rel.empty() && rel.front() == '/') {
    rel.erase(rel.begin());
  }
  if (rel.empty()) {
    return verb;
  }
  return rel + "/" + verb;
}

// The slot-0 bootstrap Aurelian serves to Agrippa over the UDS. Agrippa forwards
// a controller's leaf ask as dispatchAt{uri,verb,spec}; this resolves it against
// the sealed legion://chrome/ root via the injected ChromeDispatchFn and returns
// the serialized reply as a value (the same contract the WSS peer used). The
// real fork surgery for handle-returning navigation is deferred; leaf dispatch
// is the proven machine-fed path.
class AurelianBootstrap : public Handle {
 public:
  static std::shared_ptr<AurelianBootstrap> make(ChromeDispatchFn dispatch) {
    return std::shared_ptr<AurelianBootstrap>(
        new AurelianBootstrap(std::move(dispatch)));
  }

  velite::agentspaces::StateKind state_kind() const override {
    return velite::agentspaces::StateKind::ResolvedValue;
  }
  const Value& resolved_value() const override { return self_; }
  std::shared_ptr<Handle> resolved_handle() const override { return nullptr; }
  std::string_view broken_reason() const override { return ""; }

  std::shared_ptr<Handle> ask_impl(std::string_view msg,
                                   const Value& spec) override {
    if (msg == "dispatchAt") {
      if (!spec.is_object()) {
        return ValueHandle::make_broken("legion://errors/InvalidArgument");
      }
      const Value* uv = spec.object_get("uri");
      const Value* vv = spec.object_get("verb");
      if (!uv || !uv->is_string() || !vv || !vv->is_string()) {
        return ValueHandle::make_broken("legion://errors/InvalidArgument");
      }
      std::string reply =
          dispatch_ ? dispatch_(DerivePath(uv->as_string(), vv->as_string()))
                    : std::string("broken:no-dispatch");
      return ValueHandle::make(Value(std::move(reply)));
    }
    if (msg == "__getIdentity") {
      return ValueHandle::make(self_);
    }
    if (msg == "__getSchema") {
      return ValueHandle::make_broken("schema-undisclosed");
    }
    return ValueHandle::make_broken("legion://errors/UnknownMessage");
  }
  void tell(std::string_view, const Value&) override {}

 private:
  explicit AurelianBootstrap(ChromeDispatchFn dispatch)
      : dispatch_(std::move(dispatch)),
        self_(std::string("legion://chrome")) {}

  ChromeDispatchFn dispatch_;
  Value self_;
};

}  // namespace

struct UdsRegister::Impl {
  std::shared_ptr<velite::UdsChannel> channel;
  std::shared_ptr<Dispatcher> disp;
  std::thread serve_thread;
  std::atomic<bool> stop{false};
  std::atomic<bool> connected{false};

  void Serve() {
    auto buf = std::make_unique<uint8_t[]>(kRxBuffer);
    while (!stop.load()) {
      bool did = false;
      for (;;) {
        size_t n = 0;
        velite::ChannelError e = channel->recv(buf.get(), kRxBuffer, &n);
        if (e != velite::ChannelError::OK || n == 0) {
          break;
        }
        did = true;
        disp->on_inbound(
            std::string(reinterpret_cast<const char*>(buf.get()), n));
      }
      disp->pump_pending_answers();
      if (!did) {
        base::PlatformThread::Sleep(base::Milliseconds(2));
      }
    }
  }
};

UdsRegister::UdsRegister() : impl_(std::make_unique<Impl>()) {}

UdsRegister::~UdsRegister() {
  Stop();
}

bool UdsRegister::Start(const std::string& socket_path,
                        const std::string& facet,
                        const std::string& child_dest_hash,
                        ChromeDispatchFn dispatch) {
  impl_->channel = std::make_shared<velite::UdsChannel>();
  if (!impl_->channel->connect(socket_path.c_str())) {
    impl_->channel.reset();
    return false;
  }
  velite::UdsChannel* raw = impl_->channel.get();
  impl_->disp = std::make_shared<Dispatcher>(
      AurelianBootstrap::make(std::move(dispatch)),
      [raw](const std::string& frame) {
        raw->send(reinterpret_cast<const uint8_t*>(frame.data()), frame.size());
      });

  // The register handshake — the shipped UdsDispatcherLink::send_register frame.
  Value params = Value::make_object({
      {"facet", Value(facet)},
      {"childDestHash", Value(child_dest_hash)},
  });
  Value mount = Value::make_object({
      {"kind", Value(std::string("Mount"))},
      {"name", Value(facet)},
      {"type",
       Value(std::string("legion://types/RegisteredEmbodimentHandle"))},
      {"params", std::move(params)},
  });
  impl_->disp->emit_ask(0, "update", std::move(mount));

  impl_->connected.store(true);
  impl_->stop.store(false);
  Impl* impl = impl_.get();
  impl_->serve_thread = std::thread([impl]() { impl->Serve(); });
  return true;
}

void UdsRegister::Stop() {
  if (!impl_) {
    return;
  }
  impl_->stop.store(true);
  if (impl_->serve_thread.joinable()) {
    impl_->serve_thread.join();
  }
  if (impl_->channel) {
    impl_->channel->close();
  }
  impl_->connected.store(false);
}

bool UdsRegister::connected() const {
  return impl_ && impl_->connected.load();
}

}  // namespace aurelian
