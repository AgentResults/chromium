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
#include "velite/agentspaces-wire/json_marshal.hpp"
#include "velite/agentspaces-wire/value_handle.hpp"
#include "velite/channel.hpp"
#include "velite/uds_channel.hpp"

namespace aurelian {

namespace {

using velite::agentspaces::Handle;
using velite::agentspaces::Value;
using velite::agentspaces::ValueHandle;
using velite::agentspaces::wire::Dispatcher;

// A ResolvedHandle alias to a target — the substrate's HandlePromise shape
// (StateKind::ResolvedHandle + resolved_handle() == target), so the Dispatcher
// EXPORTS the target as a slot over the wire and a controller gets a navigable
// ref. (Local rather than velite::HandlePromise only because that header's
// function-local static trips the fork's -Wexit-time-destructors; the wire
// primitive adopted is identical.)
class NavRef : public Handle {
 public:
  static std::shared_ptr<NavRef> make(std::shared_ptr<Handle> target) {
    return std::shared_ptr<NavRef>(new NavRef(std::move(target)));
  }
  velite::agentspaces::StateKind state_kind() const override {
    return velite::agentspaces::StateKind::ResolvedHandle;
  }
  const Value& resolved_value() const override { return empty_; }
  std::shared_ptr<Handle> resolved_handle() const override { return target_; }
  std::string_view broken_reason() const override { return ""; }
  std::shared_ptr<Handle> ask_impl(std::string_view msg,
                                   const Value& spec) override {
    return target_ ? target_->ask(msg, spec)
                   : ValueHandle::make_broken("legion://errors/InternalError");
  }
  void tell(std::string_view, const Value&) override {}

 private:
  explicit NavRef(std::shared_ptr<Handle> target)
      : target_(std::move(target)) {}
  std::shared_ptr<Handle> target_;
  Value empty_;
};

constexpr size_t kRxBuffer = 65536;
constexpr char kChromeUriPrefix[] = "legion://chrome";

// HS-3 (ACM-2w, design section 5 round-5 review M5): the spec crosses the
// dispatch seam SERIALIZED and VALUE-ONLY. A re-parsed serialized spec
// cannot resolve wire slot refs (protocol.md section 4 is_slot_ref) — and an
// in-process handle ref cannot cross a string seam at all — so designation-
// bearing specs are REFUSED typed (the lossless-or-throw conversion
// doctrine), never silently flattened.
constexpr char kSlotRefInSpec[] = "legion://errors/SlotRefInSpec";

bool CarriesHandleDesignation(const Value& v) {
  if (v.is_slot_ref() || v.is_handle()) {
    return true;
  }
  if (v.is_object()) {
    for (const auto& [key, child] : v.as_object()) {
      if (CarriesHandleDesignation(child)) {
        return true;
      }
    }
    return false;
  }
  if (v.is_array()) {
    for (const Value& child : v.as_array()) {
      if (CarriesHandleDesignation(child)) {
        return true;
      }
    }
    return false;
  }
  return false;
}

// Serializes a value-only spec to its canonical-JSON wire form (empty for a
// null/absent spec). False iff the spec carries handle designation — the
// caller answers the typed refusal.
bool SerializeValueOnlySpec(const Value& spec, std::string* out) {
  if (CarriesHandleDesignation(spec)) {
    return false;
  }
  *out = spec.is_null()
             ? std::string()
             : velite::agentspaces::to_json(spec).to_string();
  return true;
}

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

// A navigable child handle bound to a legion://chrome/<uri>. Returned (slot-
// exported) by getResource so a controller can WALK the tree over the wire:
//   peer.getResource("legion://chrome/system").ask("info")   -> the leaf value
// Adopts the substrate's proven slot-ref primitive (HandlePromise -> the
// Dispatcher exports this as a slot; marius's MariusPeerHandle chains the same
// way). A leaf ask resolves against the sealed root via the injected dispatch;
// a further getResource yields a deeper NavHandle, so navigation composes.
class NavHandle : public Handle {
 public:
  static std::shared_ptr<NavHandle> make(std::string uri,
                                         ChromeDispatchFn dispatch) {
    return std::shared_ptr<NavHandle>(
        new NavHandle(std::move(uri), std::move(dispatch)));
  }

  velite::agentspaces::StateKind state_kind() const override {
    return velite::agentspaces::StateKind::ResolvedValue;
  }
  const Value& resolved_value() const override { return self_; }
  std::shared_ptr<Handle> resolved_handle() const override { return nullptr; }
  std::string_view broken_reason() const override { return ""; }

  std::shared_ptr<Handle> ask_impl(std::string_view msg,
                                   const Value& spec) override {
    if (msg == "getResource") {
      const Value* uv = spec.is_object() ? spec.object_get("uri") : nullptr;
      if (!uv || !uv->is_string()) {
        return ValueHandle::make_broken("legion://errors/InvalidArgument");
      }
      return NavRef::make(NavHandle::make(uv->as_string(), dispatch_));
    }
    if (msg == "__getIdentity") {
      return ValueHandle::make(self_);
    }
    // Any other message is a leaf verb against THIS node's uri; the
    // caller's spec rides along, serialized + value-only (HS-3).
    std::string spec_json;
    if (!SerializeValueOnlySpec(spec, &spec_json)) {
      return ValueHandle::make_broken(kSlotRefInSpec);
    }
    std::string reply =
        dispatch_ ? dispatch_(DerivePath(uri_, std::string(msg)), spec_json)
                  : std::string("broken:no-dispatch");
    return ValueHandle::make(Value(std::move(reply)));
  }
  void tell(std::string_view, const Value&) override {}

 private:
  NavHandle(std::string uri, ChromeDispatchFn dispatch)
      : uri_(std::move(uri)),
        dispatch_(std::move(dispatch)),
        self_(uri_) {}

  std::string uri_;
  ChromeDispatchFn dispatch_;
  Value self_;
};

// The slot-0 bootstrap Aurelian serves to Agrippa over the UDS. Agrippa forwards
// a controller's leaf ask as dispatchAt{uri,verb,spec}; this resolves it against
// the sealed legion://chrome/ root via the injected ChromeDispatchFn and returns
// the serialized reply as a value (the contract the WSS peer used). getResource
// {uri} additionally returns a NAVIGABLE child handle (slot-exported via
// HandlePromise) so a controller walks the tree — peer.getResource(uri).info()
// — over the register wire, adopting the substrate's proven slot-ref primitive.
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
      // HS-3: the dispatchAt frame's `spec` field (absent = no spec) crosses
      // the seam serialized + value-only.
      const Value* sv = spec.object_get("spec");
      std::string spec_json;
      if (sv && !SerializeValueOnlySpec(*sv, &spec_json)) {
        return ValueHandle::make_broken(kSlotRefInSpec);
      }
      std::string reply =
          dispatch_ ? dispatch_(DerivePath(uv->as_string(), vv->as_string()),
                                spec_json)
                    : std::string("broken:no-dispatch");
      return ValueHandle::make(Value(std::move(reply)));
    }
    if (msg == "getResource") {
      // Handle-returning navigation: return a navigable child (slot-exported)
      // bound to `uri`, so the controller can ask it (and chain further).
      const Value* uv = spec.is_object() ? spec.object_get("uri") : nullptr;
      if (!uv || !uv->is_string()) {
        return ValueHandle::make_broken("legion://errors/InvalidArgument");
      }
      return NavRef::make(NavHandle::make(uv->as_string(), dispatch_));
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
