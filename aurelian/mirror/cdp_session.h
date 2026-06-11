// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// HS-1 (ACM-2, AURELIAN-GENERIC-CONTROL-DESIGN section 3): the async CDP
// session layer — a PERSISTENT per-target client with monotonic request
// ids, request/reply correlation, and (from ACM-4) event fan-out.
// Settlement is delivered by resolving the pending answer Handle on the UI
// thread when the host's reply arrives: no nested UI loop, no blocked UI
// thread, concurrent in-flight commands on one session. This is the
// replacement for the synchronous one-shot shape in devtools_handle.cc
// (single hardcoded id, attach-wait-detach per command), which is deleted
// when its last caller migrates (design section 7 inventory row).
//
// Residency (design section 3): ONE process-global registry — persistent
// clients + in-flight correlation — the same persistence move the sealed
// root made for media_; the mirror nodes project against it.

#ifndef AURELIAN_MIRROR_CDP_SESSION_H_
#define AURELIAN_MIRROR_CDP_SESSION_H_

#include <memory>
#include <string>

#include "base/functional/callback.h"
#include "base/no_destructor.h"

namespace velite::agentspaces {
class Handle;
class Value;
}  // namespace velite::agentspaces

namespace aurelian {

// One persistent session on one target (defined in the .cc; named here so
// the registry's lazy-attach helpers can be members).
class CdpSession;

// ACM-R(5): an optional SETTLE-TIME reshaper a FOLD facade verb threads
// through its invoke — applied on the UI thread when the session layer
// settles the answer VALUE (a Broken settlement passes through untouched,
// the host's own refusal stays typed). The HS-1 layer owns settlement; the
// facade owns only the shape of its convenience reply (design section 7:
// facade verbs are semantic conveniences delegating to the mirror — no
// parallel dispatch machinery).
using CdpReshaper = base::OnceCallback<velite::agentspaces::Value(
    velite::agentspaces::Value)>;

class CdpSessionRegistry {
 public:
  // The one instance (UI-confined; design section 3 residency rule).
  static CdpSessionRegistry& Get();

  CdpSessionRegistry(const CdpSessionRegistry&) = delete;
  CdpSessionRegistry& operator=(const CdpSessionRegistry&) = delete;

  // UI thread. Dispatches `method` (qualified, "Domain.command") with
  // `params` (null => {}; an object => its canonical JSON) on the
  // persistent BROWSER-target session — lazy attach on the first invoke.
  // Returns the still-Pending answer Handle; the session layer settles it
  // on the UI thread when the reply arrives (value on result, typed broken
  // on a CDP error — the host's own answer passes through) and then calls
  // CompletionBridge::NotifySettled for any wire waiter. An attach failure
  // returns an already-Broken handle.
  std::shared_ptr<velite::agentspaces::Handle> InvokeOnBrowserTarget(
      const std::string& method,
      const velite::agentspaces::Value& params,
      CdpReshaper reshaper = {});

  // ACM-3: same contract, on the PERSISTENT session of ONE enumerated
  // target (the id is DevToolsAgentHost::GetId()'s) — lazy attach on the
  // first invoke per target; detach-on-target-close settles that session's
  // in-flight entries Broken and prunes the client (design section 3).
  std::shared_ptr<velite::agentspaces::Handle> InvokeOnTarget(
      const std::string& target_id,
      const std::string& method,
      const velite::agentspaces::Value& params,
      CdpReshaper reshaper = {});

  // UI thread. Erases target sessions whose client detached (posted by the
  // close path — never run mid-callback).
  void PruneClosedTargetSessions();

  // ACM-4: subscribe `sink` to the CDP event `event_method`
  // ("Domain.event") on the browser-target / one target's persistent
  // session — a CDP event subscription IS a handle subscription (design
  // section 3): returns the substrate SubscriptionHandle; every matching
  // protocol event reaches the sink as a `legion-notify` tell carrying
  // {event, params}. Lazy attach like invoke; the domain's `enable`
  // command (when the descriptor carries one) is dispatched on the first
  // subscription per domain — data-driven, no per-domain code.
  // `uri_prefix` (the event node's URI) mints subscription ids.
  std::shared_ptr<velite::agentspaces::Handle> SubscribeOnBrowserTarget(
      const std::string& event_method,
      std::shared_ptr<velite::agentspaces::Handle> sink,
      const std::string& uri_prefix);
  std::shared_ptr<velite::agentspaces::Handle> SubscribeOnTarget(
      const std::string& target_id,
      const std::string& event_method,
      std::shared_ptr<velite::agentspaces::Handle> sink,
      const std::string& uri_prefix);

  // The lazy-attach observables (plan ACM-2: the first invoke attaches the
  // browser session; one persistent session carries concurrent commands).
  size_t SessionCountForTesting() const;
  size_t InFlightCountForTesting() const;

  // ACM-3 observable: attached PER-TARGET sessions (the browser session is
  // not counted here). Detach-on-target-close drives this back to zero.
  size_t TargetSessionCountForTesting() const;

 private:
  friend class base::NoDestructor<CdpSessionRegistry>;

  CdpSessionRegistry();
  ~CdpSessionRegistry();

  // The shared lazy-attach halves (invoke and subscribe ride the same
  // persistent sessions). Null on attach failure. UI thread.
  CdpSession* EnsureBrowserSession();
  CdpSession* EnsureTargetSession(const std::string& target_id);

  struct Impl;  // hides the content:: client from this header
  std::unique_ptr<Impl> impl_;
};

}  // namespace aurelian

#endif  // AURELIAN_MIRROR_CDP_SESSION_H_
