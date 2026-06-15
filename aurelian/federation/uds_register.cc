// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/federation/uds_register.h"

#include "aurelian/conformance/facet_claims.h"
#include "aurelian/conformance/unified_bootstrap.h"
#include "aurelian/federation/nav_handle_internal.h"
#include "aurelian/federation/pending_dispatch.h"
#include "aurelian/federation/serve_pump.h"
#include "aurelian/federation/wire_event_mailbox.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <thread>
#include <utility>

#include "aurelian/capability/cap_gate.h"
#include "aurelian/capability/cap_wire.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "velite/agentspaces/limits.hpp"
#include "velite/agentspaces-wire/dispatcher.hpp"
#include "velite/agentspaces-wire/handle.hpp"
#include "velite/agentspaces-wire/json_marshal.hpp"
#include "velite/agentspaces-wire/peer_session.hpp"
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

// ACM-9p (design section 8): the caller buffer is sized to the wire MTU so
// every spec-legal envelope (64 KB, 1 MiB] the fixed channel stages can be
// DELIVERED — the 64 KB wedge was the caller buffer, not the channel. The
// buffer lives ON THE HEAP (Serve() make_unique) per the G3 caveat: a 1 MiB
// serve-thread stack buffer would repeat the wss_peer 63230cc169 overflow.
constexpr size_t kRxBuffer = velite::VELITE_CHANNEL_MAX_ENVELOPE_BYTES;
constexpr char kChromeUriPrefix[] = "legion://chrome";

// CF-6: the wire dispatch deadline (was the blocking bridge's wait budget).
// A never-settling chrome dispatch answers the typed timeout via the
// pending handle's state_kind() — no timer thread, no serve-thread block.
constexpr base::TimeDelta kWireDispatchDeadline = base::Seconds(15);

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

// CF-8 (design §7): the ONE operator cap trust anchor, shared by every
// chrome-surface seam — the dispatchAt facade, getResource, bound NavHandle
// asks, and the wire subscribe all gate against THIS. No anchor → presented
// caps fail closed (GateFederationDispatch refuses cap-untrusted-anchor).
struct GateAnchor {
  PubKey pub{};
  bool present = false;
};

std::shared_ptr<const GateAnchor> MakeGateAnchor(
    const std::vector<uint8_t>& bytes) {
  auto anchor = std::make_shared<GateAnchor>();
  if (bytes.size() == anchor->pub.size()) {
    std::copy(bytes.begin(), bytes.end(), anchor->pub.begin());
    anchor->present = true;
  }
  return anchor;
}

// CF-8: AND-only chain composition (constraints.md §5 — attenuation can only
// narrow): EVERY bound/presented chain must admit (verb, uri); the first
// refusal is THE typed answer. `now` is re-evaluated per call — caps expire,
// so the predicate decision is never cached (the chain's signature check is
// cacheable; this seam keeps the call uniform instead).
std::string GateChains(const std::vector<std::string>& chains,
                       const GateAnchor& anchor,
                       const std::string& canonical_uri,
                       const std::string& verb) {
  const int64_t now = base::Time::Now().ToTimeT();
  for (const std::string& chain : chains) {
    std::string denied = GateFederationDispatch(
        chain, anchor.pub, anchor.present, canonical_uri, verb, now);
    if (!denied.empty()) {
      return denied;
    }
  }
  return std::string();
}

// The minimum nonzero link expiry of a serialized chain — the per-delivery
// re-check input for wire subscriptions (design §7: under operator-anchor
// caps the per-delivery re-check reduces to expiry). 0 = no expiry.
int64_t ChainMinExpiry(const std::string& serialized_chain) {
  std::vector<CapLink> chain;
  if (!ParseChain(serialized_chain, &chain)) {
    return 0;
  }
  int64_t expires = 0;
  for (const CapLink& link : chain) {
    if (link.expires != 0 && (expires == 0 || link.expires < expires)) {
      expires = link.expires;
    }
  }
  return expires;
}

// ACM-8: the canonical ABSOLUTE form of a dispatchAt uri, derived by the
// exact stripping DerivePath applies — so the cap gate always matches the
// same resource the dispatch resolves, whichever spelling (absolute /
// relative) the caller used.
std::string CanonicalGateUri(const std::string& uri) {
  std::string rel;
  if (uri.compare(0, sizeof(kChromeUriPrefix) - 1, kChromeUriPrefix) == 0) {
    rel = uri.substr(sizeof(kChromeUriPrefix) - 1);
  } else {
    rel = uri;
  }
  while (!rel.empty() && rel.front() == '/') {
    rel.erase(rel.begin());
  }
  if (rel.empty()) {
    return kChromeUriPrefix;
  }
  return std::string(kChromeUriPrefix) + "/" + rel;
}

// A navigable child handle bound to a legion://chrome/<uri>. Returned (slot-
// exported) by getResource so a controller can WALK the tree over the wire:
//   peer.getResource("legion://chrome/system").ask("info")   -> the leaf value
// Adopts the substrate's proven slot-ref primitive (HandlePromise -> the
// Dispatcher exports this as a slot; marius's MariusPeerHandle chains the same
// way). A leaf ask resolves against the sealed root via the injected dispatch;
// a further getResource yields a deeper NavHandle, so navigation composes.
//
// CF-8 (design §7): the handle CARRIES THE ATTENUATION IT WAS ACQUIRED UNDER
// — `chains_` is the verified cap set the acquisition presented/inherited
// (empty = connection-level authority, the status quo). Every ask re-runs
// the ONE evaluator over CanonicalGateUri(uri_) + verb (`now` re-evaluated:
// caps expire between acquisition and ask); child minting composes AND-only
// (parent ∧ presented) and is itself gated over the CHILD uri, so
// attenuation can only narrow.
class NavHandle : public Handle {
 public:
  static std::shared_ptr<NavHandle> make(
      std::string uri,
      BeginChromeDispatchFn dispatch,
      std::shared_ptr<const GateAnchor> anchor,
      std::vector<std::string> chains) {
    return std::shared_ptr<NavHandle>(new NavHandle(
        std::move(uri), std::move(dispatch), std::move(anchor),
        std::move(chains)));
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
      // CF-8: parent ∧ presented — the child inherits every bound chain
      // plus the presented one, and the acquisition itself is gated over
      // the CHILD uri against ALL of them (a wider presented cap cannot
      // escape the parent's subtree).
      std::vector<std::string> child_chains = chains_;
      const Value* capv = spec.object_get("cap");
      if (capv) {
        if (!capv->is_string()) {
          return ValueHandle::make_broken("cap-refused:cap-chain-malformed");
        }
        child_chains.push_back(capv->as_string());
      }
      std::string denied =
          GateChains(child_chains, *anchor_,
                     CanonicalGateUri(uv->as_string()), "getResource");
      if (!denied.empty()) {
        return ValueHandle::make_broken(denied);
      }
      return NavRef::make(NavHandle::make(uv->as_string(), dispatch_,
                                          anchor_, std::move(child_chains)));
    }
    // CF-8: EVERY other ask on a bound handle re-runs the gate per ask —
    // __getIdentity included (it is a leaf ask; verbs classify read via
    // the __get prefix, and an expired chain refuses here too).
    std::string denied =
        GateChains(chains_, *anchor_, CanonicalGateUri(uri_),
                   std::string(msg));
    if (!denied.empty()) {
      return ValueHandle::make_broken(denied);
    }
    if (msg == "__getIdentity") {
      return ValueHandle::make(self_);
    }
    // Any other message is a leaf verb against THIS node's uri; the
    // caller's spec rides along, serialized + value-only (HS-3 — the typed
    // refusal runs HERE, synchronously on the serve thread, before the
    // begin-form is reached; design §6.1). CF-6: the dispatch DEFERS — the
    // begin-form creates the record pre-post and the returned
    // PendingDispatchHandle settles through the dispatcher pump (F8b).
    std::string spec_json;
    if (!SerializeValueOnlySpec(spec, &spec_json)) {
      return ValueHandle::make_broken(kSlotRefInSpec);
    }
    if (!dispatch_) {
      return ValueHandle::make_broken("broken:no-dispatch");
    }
    std::shared_ptr<CompletionRecord> record =
        dispatch_(DerivePath(uri_, std::string(msg)), spec_json);
    if (!record) {
      return ValueHandle::make_broken("broken:no-dispatch");
    }
    return PendingDispatchHandle::make(
        std::move(record), base::TimeTicks::Now() + kWireDispatchDeadline);
  }
  void tell(std::string_view, const Value&) override {}

 private:
  NavHandle(std::string uri,
            BeginChromeDispatchFn dispatch,
            std::shared_ptr<const GateAnchor> anchor,
            std::vector<std::string> chains)
      : uri_(std::move(uri)),
        dispatch_(std::move(dispatch)),
        anchor_(std::move(anchor)),
        chains_(std::move(chains)),
        self_(uri_) {}

  std::string uri_;
  BeginChromeDispatchFn dispatch_;
  std::shared_ptr<const GateAnchor> anchor_;
  std::vector<std::string> chains_;
  Value self_;
};

// CF-4 (design §4.2) — the thin slot-0 wrapper: COMPOSES the unified
// vendored bootstrap (delegate-everything — the canonical surface, mount
// table, claims cap all live there) and intercepts exactly TWO message
// families as the chrome-mount navigation facade:
//
//  - dispatchAt{uri,verb,spec?,cap?}: the kept keystone/Agrippa verb,
//    reimplemented as a FOLD facade over the mounted chrome root — the
//    ACM-8 cap gate runs FIRST (authority before arguments, semantics
//    unchanged), then the ask resolves through the same NavHandle the
//    mount table serves (HS-3 serialized value-only spec, the one
//    wire-serialize reply path). Deleting it later costs one row.
//  - getResource{uri} for chrome-SUBTREE uris: resolves THROUGH the
//    mounted root (a child NavHandle, slot-exported via NavRef) — the
//    mount's own navigation semantic; every other uri delegates to the
//    vendored resolver ([EMBODIMENT-RESOLVER-IS-MOUNT-TABLE]).
//
// The wrapper owns no mount table, no verb table beyond the facade, no
// state (the generic-control FOLD discipline). AurelianBootstrap (the
// four-verb shim) is DELETED with this replacement — inventory row.
class AurelianSlotZero : public Handle {
 public:
  static std::shared_ptr<AurelianSlotZero> make(
      std::shared_ptr<velite::agentspaces::ConformanceBootstrap> vendored,
      BeginChromeDispatchFn dispatch,
      std::shared_ptr<const GateAnchor> anchor,
      ChromeWireSubscribeFn wire_subscribe = {},
      WireEventMailbox* mailbox = nullptr) {
    return std::shared_ptr<AurelianSlotZero>(new AurelianSlotZero(
        std::move(vendored), std::move(dispatch), std::move(anchor),
        std::move(wire_subscribe), mailbox));
  }

  velite::agentspaces::StateKind state_kind() const override {
    return vendored_->state_kind();
  }
  const Value& resolved_value() const override {
    return vendored_->resolved_value();
  }
  std::shared_ptr<Handle> resolved_handle() const override {
    return vendored_->resolved_handle();
  }
  std::string_view broken_reason() const override {
    return vendored_->broken_reason();
  }

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
      // ACM-8: authority before arguments — a presented cap is verified to
      // the operator anchor and its effective predicate enforced on the
      // CANONICAL full URI; a refused dispatch never runs. Cap absent =
      // the connection's facet-level authority (status quo).
      const Value* capv = spec.object_get("cap");
      if (capv) {
        if (!capv->is_string()) {
          return ValueHandle::make_broken("cap-refused:cap-chain-malformed");
        }
        std::string denied = GateChains({capv->as_string()}, *anchor_,
                                        CanonicalGateUri(uv->as_string()),
                                        vv->as_string());
        if (!denied.empty()) {
          return ValueHandle::make_broken(denied);
        }
      }
      // The FOLD body — the same seam a mounted-root leaf ask runs
      // (NavHandle::ask_impl's leaf path verbatim): HS-3 value-only
      // serialization (slot-ref specs refused typed, synchronously on the
      // serve thread) + the ONE begin-form over DerivePath. EVERY verb
      // dispatches — __getIdentity included — preserving the keystone wire
      // shape ("legion://chrome/" comes from the sealed root, never from a
      // navigation-layer intercept). CF-6: the dispatch DEFERS (F8b).
      const Value* sv = spec.object_get("spec");
      std::string spec_json;
      if (sv && !SerializeValueOnlySpec(*sv, &spec_json)) {
        return ValueHandle::make_broken(kSlotRefInSpec);
      }
      if (!dispatch_) {
        return ValueHandle::make_broken("broken:no-dispatch");
      }
      std::shared_ptr<CompletionRecord> record =
          dispatch_(DerivePath(uv->as_string(), vv->as_string()), spec_json);
      if (!record) {
        return ValueHandle::make_broken("broken:no-dispatch");
      }
      return PendingDispatchHandle::make(
          std::move(record), base::TimeTicks::Now() + kWireDispatchDeadline);
    }
    if (msg == "legion-subscribe-remote") {
      // CF-5 (design §5.2/§5.4): a chrome event-node URI routes through the
      // mirror's subscribe surface via the installed wire-subscribe seam —
      // the producer-side WireSinkHandle is constructed there (the sub_id
      // relay; no designation in the spec), the registration posts to the
      // UI thread, and events come back through the mailbox the serve
      // loop drains. Non-chrome URIs keep the vendored KG path untouched.
      const Value* uv = spec.is_object() ? spec.object_get("uri") : nullptr;
      const Value* sid = spec.is_object() ? spec.object_get("sub_id") : nullptr;
      if (uv && uv->is_string() &&
          uv->as_string().compare(0, sizeof(kChromeUriPrefix) - 1,
                                  kChromeUriPrefix) == 0) {
        if (!sid || !sid->is_string()) {
          return ValueHandle::make_broken("legion://errors/InvalidArgument");
        }
        // CF-8 (design §7): the wire subscribe is an ask and passes the
        // SAME gate BEFORE any registration (subscribe.md §5 at-subscribe-
        // time intersection). The chain's minimum expiry feeds the
        // per-delivery re-check (the mailbox drops post-expiry events and
        // terminates the subscription typed).
        int64_t cap_expires = 0;
        const Value* capv = spec.object_get("cap");
        if (capv) {
          if (!capv->is_string()) {
            return ValueHandle::make_broken(
                "cap-refused:cap-chain-malformed");
          }
          std::string denied = GateChains(
              {capv->as_string()}, *anchor_,
              CanonicalGateUri(uv->as_string()), "legion-subscribe-remote");
          if (!denied.empty()) {
            return ValueHandle::make_broken(denied);
          }
          cap_expires = ChainMinExpiry(capv->as_string());
        }
        if (!wire_subscribe_ || !mailbox_) {
          return ValueHandle::make_broken("wire-eventing-unavailable");
        }
        if (cap_expires != 0) {
          mailbox_->SetSubscriptionExpiry(sid->as_string(), cap_expires);
        }
        // CF-6: subscribe is just an ask — its ack rides the SAME deferred
        // pending path as every chrome dispatch (no special-casing in the
        // end state, design §5.2).
        std::shared_ptr<CompletionRecord> record =
            wire_subscribe_(uv->as_string(), sid->as_string(), mailbox_);
        if (!record) {
          return ValueHandle::make_broken("wire-eventing-unavailable");
        }
        return PendingDispatchHandle::make(
            std::move(record),
            base::TimeTicks::Now() + kWireDispatchDeadline);
      }
      return vendored_->ask(msg, spec);
    }
    if (msg == "getResource") {
      // Chrome-subtree navigation resolves THROUGH the mount: a relative or
      // chrome-prefixed uri mints the navigable child (slot-exported).
      // legion://chrome itself and every non-chrome uri delegate to the
      // vendored resolver (mount table, type URIs, claims-capped
      // facilities).
      const Value* uv = spec.is_object() ? spec.object_get("uri") : nullptr;
      if (uv && uv->is_string()) {
        const std::string& uri = uv->as_string();
        const bool chrome_subtree =
            uri.compare(0, sizeof(kChromeUriPrefix) - 1, kChromeUriPrefix) ==
                0 ||
            uri.rfind("legion://", 0) != 0;  // relative form
        if (chrome_subtree && uri != kChromeUriPrefix) {
          // CF-8 (design §7): a presented cap is verified to the anchor and
          // enforced over the CANONICAL target uri at acquisition; the
          // returned handle is BORN BOUND to the verified chain. Absent:
          // connection-level authority (status quo).
          std::vector<std::string> chains;
          const Value* capv = spec.object_get("cap");
          if (capv) {
            if (!capv->is_string()) {
              return ValueHandle::make_broken(
                  "cap-refused:cap-chain-malformed");
            }
            chains.push_back(capv->as_string());
            std::string denied = GateChains(
                chains, *anchor_, CanonicalGateUri(uri), "getResource");
            if (!denied.empty()) {
              return ValueHandle::make_broken(denied);
            }
          }
          return NavRef::make(
              NavHandle::make(uri, dispatch_, anchor_, std::move(chains)));
        }
      }
      return vendored_->ask(msg, spec);
    }
    return vendored_->ask(msg, spec);
  }
  void tell(std::string_view msg, const Value& data) override {
    vendored_->tell(msg, data);
  }

 private:
  AurelianSlotZero(
      std::shared_ptr<velite::agentspaces::ConformanceBootstrap> vendored,
      BeginChromeDispatchFn dispatch,
      std::shared_ptr<const GateAnchor> anchor,
      ChromeWireSubscribeFn wire_subscribe,
      WireEventMailbox* mailbox)
      : vendored_(std::move(vendored)),
        dispatch_(std::move(dispatch)),
        anchor_(std::move(anchor)),
        wire_subscribe_(std::move(wire_subscribe)),
        mailbox_(mailbox) {}

  std::shared_ptr<velite::agentspaces::ConformanceBootstrap> vendored_;
  BeginChromeDispatchFn dispatch_;
  // ACM-8/CF-8: the operator cap trust anchor (the SAME anchor the renderer
  // membranes are provisioned with), shared with every NavHandle this
  // surface mints. No anchor → presented caps fail closed.
  std::shared_ptr<const GateAnchor> anchor_;
  ChromeWireSubscribeFn wire_subscribe_;
  WireEventMailbox* mailbox_ = nullptr;
};

}  // namespace

// CF-1 (nav_handle_internal.h): the ONE NavHandle implementation exported to
// the conformance serve for the legion://chrome mount — no parallel handle.
// CF-8: carries the anchor so presented caps verify on this bring-up too.
std::shared_ptr<Handle> MakeChromeNavHandle(
    const std::string& uri,
    BeginChromeDispatchFn dispatch,
    const std::vector<uint8_t>& cap_anchor) {
  return NavHandle::make(uri, std::move(dispatch), MakeGateAnchor(cap_anchor),
                         {});
}

// CF-4 (nav_handle_internal.h): the ONE slot-0 wrapper, exported so the
// conformance serve exhibits the SAME surface (design §4.1).
std::shared_ptr<Handle> MakeUnifiedSlotZero(
    std::shared_ptr<velite::agentspaces::ConformanceBootstrap> vendored,
    BeginChromeDispatchFn dispatch,
    const std::vector<uint8_t>& cap_anchor,
    ChromeWireSubscribeFn wire_subscribe,
    WireEventMailbox* mailbox) {
  return AurelianSlotZero::make(std::move(vendored), std::move(dispatch),
                                MakeGateAnchor(cap_anchor),
                                std::move(wire_subscribe), mailbox);
}

struct UdsRegister::Impl {
  std::shared_ptr<velite::UdsChannel> channel;
  std::shared_ptr<Dispatcher> disp;
  std::thread serve_thread;
  std::atomic<bool> stop{false};
  std::atomic<bool> connected{false};
  std::string dest_hash;
  // CF-5: the wire-event mailbox — UI-thread producers (WireSinkHandle),
  // serve-thread drain (the flush hook below).
  WireEventMailbox mailbox;

  // The ONE serve loop (serve_pump.h, design §4.1) over the envelope-framed
  // channel drain. The register-in stays RESIDENT across session close
  // (production: lifetime is the browser's, not the connection's — the
  // conformance serve is the mode whose lifetime IS the connection), and
  // keeps DRAINING the channel after a typed close so a refused oversize
  // body keeps discarding instead of wedging the sender (the ACM-9p
  // framing-preserved contract; CF-4 found the wedge when an early exit
  // stopped the reads). Only a hard channel close ends the loop.
  void Serve() {
    auto buf = std::make_unique<uint8_t[]>(kRxBuffer);
    RunServeLoop(
        stop, *disp,
        [this, &buf]() {
          bool did = false;
          for (;;) {
            size_t n = 0;
            velite::ChannelError e = channel->recv(buf.get(), kRxBuffer, &n);
            if (e == velite::ChannelError::MessageTooLarge) {
              // ACM-9p: the channel refused an above-MTU envelope at header
              // time (body drained, framing preserved); the session layer
              // answers the spec's typed refusal — limits.md section 2
              // CLOSE{frame-too-large}.
              disp->emit_close(std::string(
                  velite::agentspaces::limits::kReasonFrameTooLarge));
              did = true;  // keep draining: the discard path needs reads
              continue;
            }
            if (e == velite::ChannelError::Closed) {
              return ServeDrain::kEnded;  // hub gone — the serve ends
            }
            if (e != velite::ChannelError::OK || n == 0) {
              break;
            }
            did = true;
            disp->on_inbound(
                std::string(reinterpret_cast<const char*>(buf.get()), n));
          }
          return did ? ServeDrain::kProgress : ServeDrain::kIdle;
        },
        []() { base::PlatformThread::Sleep(base::Milliseconds(2)); },
        // CF-5: drain the wire-event mailbox beside the pending pump — the
        // ONE dispatcher emission path, serve-thread confined (design §5.2;
        // the frame is byte-identical to SubSinkHandle::tell's by
        // construction: same verb, same payload shape, same emit_tell).
        [this]() {
          for (Value& entry : mailbox.DrainAll()) {
            disp->emit_tell(0, std::string("legion-subscription-event"),
                            std::move(entry));
          }
        });
  }
};

UdsRegister::UdsRegister() : impl_(std::make_unique<Impl>()) {}

UdsRegister::~UdsRegister() {
  Stop();
}

bool UdsRegister::Start(const std::string& socket_path,
                        const std::string& facet,
                        const std::string& peer_seed,
                        BeginChromeDispatchFn dispatch,
                        const std::vector<uint8_t>& cap_anchor,
                        ChromeWireSubscribeFn wire_subscribe) {
  impl_->channel = std::make_shared<velite::UdsChannel>();
  if (!impl_->channel->connect(socket_path.c_str())) {
    impl_->channel.reset();
    return false;
  }
  // CF-4 (design §4.1/§4.2): the UNIFIED bootstrap — the same construction
  // the conformance serve runs (seeded identity, chrome mounted pre-seal,
  // claims cap) — composed with the thin dispatchAt facade. Two bring-up
  // modes, ONE surface. CF-8: ONE anchor, shared by the mounted root and
  // the wrapper (every navigation seam gates against the same trust root).
  auto anchor = MakeGateAnchor(cap_anchor);
  auto vendored = BuildUnifiedBootstrap(
      peer_seed, NavHandle::make(kChromeUriPrefix, dispatch, anchor, {}));
  velite::UdsChannel* raw = impl_->channel.get();
  impl_->disp = std::make_shared<Dispatcher>(
      AurelianSlotZero::make(vendored, std::move(dispatch), std::move(anchor),
                             std::move(wire_subscribe), &impl_->mailbox),
      [raw](const std::string& frame) {
        raw->send(reinterpret_cast<const uint8_t*>(frame.data()), frame.size());
      });
  auto bootstrap = vendored;
  impl_->disp->set_audit([bootstrap](std::string event, std::string trace_id) {
    bootstrap->record_audit(std::move(event), std::move(trace_id));
  });
  vendored->set_counterparty_dispatcher(impl_->disp.get());

  // The browser's REAL identity ([EMBODIMENT-REGISTERED-CHILD-OWN-IDENTITY]):
  // the seeded Ed25519 destHash, read off the unified bootstrap's own
  // identity surface.
  auto ident = vendored->ask("getOrgIdentity", Value());
  if (ident &&
      ident->state_kind() == velite::agentspaces::StateKind::ResolvedValue) {
    if (const Value* dh = ident->resolved_value().object_get("destHash");
        dh && dh->is_string()) {
      impl_->dest_hash = dh->as_string();
    }
  }

  // The register handshake — the shipped UdsDispatcherLink::send_register frame.
  Value params = Value::make_object({
      {"facet", Value(facet)},
      {"childDestHash", Value(impl_->dest_hash)},
  });
  Value mount = Value::make_object({
      {"kind", Value(std::string("Mount"))},
      {"name", Value(facet)},
      {"type",
       Value(std::string("legion://types/RegisteredEmbodimentHandle"))},
      {"params", std::move(params)},
  });
  impl_->disp->emit_ask(0, "update", std::move(mount));

  // sessions.md §2a — the register-in peer is the CONNECTOR: declare Aurelian's
  // OWN facet manifest, then emit a propose-session carrying it on THIS wire (the
  // responder — Agrippa — answers with its ack-session in on_inbound). The legacy
  // proactive __handshake emitter (emit_session_handshake_with_facets) is retired;
  // set_local_facet_claims makes the propose advertise Aurelian, not velite-cpp.
  impl_->disp->set_local_facet_claims(aurelian_claimed_wire_facets(),
                                      aurelian_claimed_internal_facets(),
                                      "legion://peers/aurelian");
  impl_->disp->emit_propose_session();

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

const std::string& UdsRegister::dest_hash() const {
  return impl_->dest_hash;
}

}  // namespace aurelian
