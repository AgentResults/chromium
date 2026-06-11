// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// CF-5 — the wire-event thread bridge (conformant-federation design §5.2),
// the ONE new mechanism in the wire-eventing feature: CDP events arrive on
// the UI thread; the Dispatcher (emission included) is serve-thread
// confined. The bridge is the dual of the HS-1 completion record:
//
//  - WireEventMailbox: a mutex-guarded queue of plain-data Value entries
//    ({sub_id, msg, event} — pure protocol JSON; the sub_id relay keeps
//    designation out of the payload). The handoff is move-under-the-mutex:
//    the producer constructs and moves in, the drain moves out — exclusive
//    ownership transfers at the lock; no Value is ever SHARED across
//    threads. Bounded per subscription (limits.md-sourced default);
//    overflow terminates the subscription with the typed
//    legion://errors/SubscriptionOverflow and a final cancelled notify —
//    never silent loss.
//  - WireSinkHandle: the sink the wire-subscribe registration hands to
//    CdpSessionRegistry::Subscribe* — SubSinkHandle's shape with the
//    inline emit_tell replaced by the enqueue (the serve loop drains and
//    emits on the serve thread via the ONE dispatcher emission path).
//
// Velite-typed INTERNAL header (the nav_handle_internal.h discipline).

#ifndef AURELIAN_FEDERATION_WIRE_EVENT_MAILBOX_H_
#define AURELIAN_FEDERATION_WIRE_EVENT_MAILBOX_H_

#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "base/synchronization/lock.h"
#include "velite/agentspaces-wire/handle.hpp"

namespace aurelian {

class WireEventMailbox {
 public:
  enum class PushResult {
    kOk,             // queued
    kOverflowedNow,  // this push crossed the cap: terminal notify queued,
                     // the subscription must be cancelled by the caller
    kDropped,        // the subscription already overflowed — entry dropped
  };

  WireEventMailbox();
  ~WireEventMailbox();

  WireEventMailbox(const WireEventMailbox&) = delete;
  WireEventMailbox& operator=(const WireEventMailbox&) = delete;

  // Producer (UI) thread: move `entry` ({sub_id, msg, event}) in under the
  // lock. On crossing the per-subscription cap, queues the ONE terminal
  // cancelled notify ({sub_id, msg: legion-notify, event: {state:
  // "cancelled", reason: legion://errors/SubscriptionOverflow}}) instead
  // and marks the subscription dead (further pushes drop).
  PushResult Push(const std::string& sub_id,
                  velite::agentspaces::Value entry);

  // Serve thread: move every queued entry out (arrival order).
  std::vector<velite::agentspaces::Value> DrainAll();

  // CF-8 (design §7): a subscription admitted under an EXPIRING cap records
  // its expiry here at registration; the delivery path re-evaluates `now` —
  // a post-expiry Push queues the ONE typed terminal cancelled notify
  // (reason cap-refused:cap-expired) instead of the event and marks the
  // subscription dead (the caller cancels the producer side, the overflow
  // lifecycle). 0 = no expiry.
  void SetSubscriptionExpiry(const std::string& sub_id, int64_t expires_unix);

  // The overflow bound (default: the limits.md queue-depth bound).
  void SetPerSubscriptionCapForTesting(size_t cap);

  // Pre-sets the cap for mailboxes CONSTRUCTED AFTER this call — the boot
  // path owns its mailbox (no instance accessor), so an overflow test sets
  // this in its fixture before the browser boots. SIZE_MAX = unset.
  static void SetDefaultPerSubscriptionCapForTesting(size_t cap);

 private:
  base::Lock lock_;
  std::deque<velite::agentspaces::Value> queue_;
  std::map<std::string, size_t> queued_per_sub_;
  std::map<std::string, int64_t> expiry_per_sub_;
  std::set<std::string> overflowed_;
  size_t per_sub_cap_;
};

// The producer-constructed wire sink (design §5.4: the WIRE sink is never a
// slot-ref crossing the serialized seam). tell("legion-notify", event) on
// the UI thread builds the {sub_id, msg, event} entry and enqueues; on
// overflow it cancels the underlying mirror subscription (UI thread — the
// fan-out delivers here on UI).
class WireSinkHandle : public velite::agentspaces::Handle {
 public:
  static std::shared_ptr<WireSinkHandle> make(std::string sub_id,
                                              WireEventMailbox* mailbox);

  velite::agentspaces::StateKind state_kind() const override;
  const velite::agentspaces::Value& resolved_value() const override;
  std::shared_ptr<velite::agentspaces::Handle> resolved_handle()
      const override;
  std::string_view broken_reason() const override;
  std::shared_ptr<velite::agentspaces::Handle> ask_impl(
      std::string_view msg, const velite::agentspaces::Value& spec) override;
  void tell(std::string_view msg,
            const velite::agentspaces::Value& data) override;

  // The mirror SubscriptionHandle, bound after registration so overflow can
  // cancel it (tell("cancel") — the SubscriptionProducer lifecycle).
  void set_subscription(std::shared_ptr<velite::agentspaces::Handle> sub);

 private:
  WireSinkHandle(std::string sub_id, WireEventMailbox* mailbox);

  const std::string sub_id_;
  WireEventMailbox* const mailbox_;
  std::shared_ptr<velite::agentspaces::Handle> subscription_;
  velite::agentspaces::Value self_;
};

}  // namespace aurelian

#endif  // AURELIAN_FEDERATION_WIRE_EVENT_MAILBOX_H_
