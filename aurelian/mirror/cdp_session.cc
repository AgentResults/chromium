// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/mirror/cdp_session.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>

#include "aurelian/catalog/cdp_catalog.h"
#include "aurelian/federation/completion_bridge.h"
#include "aurelian/handles/streams/subscription_producer.h"
#include "aurelian/mirror/cdp_agent_client.h"
#include "aurelian/mirror/value_convert.h"
#include "base/check.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/memory/weak_ptr.h"
#include "base/task/single_thread_task_runner.h"
#include "base/values.h"
#include "content/public/browser/browser_thread.h"
#include "velite/agentspaces-wire/handle.hpp"
#include "velite/agentspaces-wire/json_marshal.hpp"
#include "velite/agentspaces-wire/value_handle.hpp"

namespace aurelian {

namespace {

using velite::agentspaces::Handle;
using velite::agentspaces::StateKind;
using velite::agentspaces::Value;
using velite::agentspaces::ValueHandle;

// The pending answer handle (the four-state contract): Pending until the
// session layer settles it on the UI thread — to the command's result value,
// or Broken (a CDP error passes the host's own answer through, typed; a
// closing target settles Broken per design section 3 detach-on-close).
class PendingCdpAnswer : public Handle {
 public:
  static std::shared_ptr<PendingCdpAnswer> make() {
    return std::shared_ptr<PendingCdpAnswer>(new PendingCdpAnswer());
  }

  StateKind state_kind() const override { return state_; }
  const Value& resolved_value() const override { return value_; }
  std::shared_ptr<Handle> resolved_handle() const override { return nullptr; }
  std::string_view broken_reason() const override { return reason_; }
  // A transient answer, not an addressable mount — not persistable.
  std::string sturdy_identity() const override { return std::string(); }

  std::shared_ptr<Handle> ask_impl(std::string_view /*msg*/,
                                   const Value& /*spec*/) override {
    return ValueHandle::make_broken(
        state_ == StateKind::Pending ? "answer-pending" : "unknown-message");
  }
  void tell(std::string_view, const Value&) override {}

  void SettleValue(Value v) {
    DCHECK(state_ == StateKind::Pending);
    value_ = std::move(v);
    state_ = StateKind::ResolvedValue;
  }

  void SettleBroken(std::string reason) {
    DCHECK(state_ == StateKind::Pending);
    reason_ = std::move(reason);
    state_ = StateKind::Broken;
  }

 private:
  PendingCdpAnswer() = default;

  StateKind state_ = StateKind::Pending;
  Value value_;
  std::string reason_;
};

}  // namespace

// ONE persistent session on one target: a long-lived client (the no-RTTI
// content glue, cdp_agent_client) with monotonic request ids and an
// id-keyed in-flight correlation map (design section 3 — the replacement
// for the attach-wait-detach single-id one-shot). UI-confined. Named (not
// file-local) so the registry's lazy-attach helpers can be members; the
// definition lives only in this TU.
//
// The protocol send is POSTED, not inline (worklog ACM-2): the fork's
// browser-session host delivers the client callback ON THE DISPATCH STACK
// (devtools_session.cc DispatchProtocolMessageToClient calls the client
// directly — no post), so an inline send would settle synchronous commands
// inside ask_impl, falsifying the design's premise that a completion
// arrives in a later UI turn and making the plan's two-in-flight RED
// unsatisfiable. One posted task per command keeps the contract uniform:
// the invoke turn returns a Pending handle; settlement ALWAYS arrives in a
// later turn via the client callback; no re-entrant settlement under a
// mirror node's ask.
class CdpSession {
 public:
  CdpSession() = default;

  bool AttachToBrowserTarget() {
    client_ = CdpAgentClient::CreateForBrowserTarget(
        base::BindRepeating(&CdpSession::OnMessage, base::Unretained(this)),
        base::BindOnce(&CdpSession::OnClosed, base::Unretained(this)));
    return client_->Attach();
  }

  // ACM-3: one persistent session per enumerated target.
  bool AttachToTarget(const std::string& target_id) {
    client_ = CdpAgentClient::CreateForTargetId(
        target_id,
        base::BindRepeating(&CdpSession::OnMessage, base::Unretained(this)),
        base::BindOnce(&CdpSession::OnClosed, base::Unretained(this)));
    return client_->Attach();
  }

  // Runs AFTER OnClosed settled the in-flight entries (the registry posts
  // its prune from here — never erases a session mid-callback).
  void set_on_closed(base::OnceClosure on_closed) {
    on_closed_external_ = std::move(on_closed);
  }

  bool attached() const { return client_ && client_->attached(); }
  size_t in_flight_count() const { return in_flight_.size(); }

  std::shared_ptr<Handle> Invoke(const std::string& method,
                                 const Value& params) {
    std::string params_json;
    if (params.is_null()) {
      params_json = "{}";
    } else if (params.is_object()) {
      params_json = velite::agentspaces::to_json(params).to_string();
    } else {
      return ValueHandle::make_broken("invalid-params:not-an-object");
    }

    std::shared_ptr<PendingCdpAnswer> answer = PendingCdpAnswer::make();
    const int id = next_id_++;
    in_flight_.emplace(id, answer);
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&CdpSession::SendFrame, weak_factory_.GetWeakPtr(),
                       "{\"id\":" + std::to_string(id) + ",\"method\":\"" +
                           method + "\",\"params\":" + params_json + "}"));
    return answer;
  }

  // ACM-4: subscribe `sink` to `event_method` on THIS session. The fan-out
  // transport is the substrate SubscriptionProducer (the section-7 KEEP
  // row this slice consumes): one producer per event method, broadcasting
  // every matching protocol event as a `legion-notify` frame. The domain's
  // `enable` command is dispatched on the first subscription per domain
  // when the descriptor carries one — catalog-driven, no per-domain code.
  std::shared_ptr<Handle> Subscribe(const std::string& event_method,
                                    std::shared_ptr<Handle> sink,
                                    const std::string& uri_prefix) {
    const size_t dot = event_method.find('.');
    const std::string domain = event_method.substr(0, dot);
    if (enabled_domains_.insert(domain).second &&
        CdpCatalog::Get().FindCommand(domain, "enable")) {
      // Fire-and-forget on this session; the answer settles like any
      // command's, with no waiter attached.
      Invoke(domain + ".enable", Value());
    }
    std::unique_ptr<SubscriptionProducer>& producer =
        producers_[event_method];
    if (!producer) {
      producer = std::make_unique<SubscriptionProducer>();
    }
    return producer->Subscribe(std::move(sink), uri_prefix, std::string());
  }

 private:
  // The posted send (UI thread, the turn after the invoke). A session torn
  // down in between already settled its in-flight entries Broken.
  void SendFrame(const std::string& frame) {
    if (client_ && client_->attached()) {
      client_->Send(frame);
    }
  }

  // The persistent client's message callback (UI thread): correlate by id,
  // settle, notify any wire waiter.
  void OnMessage(const std::string& message) {
    std::optional<base::DictValue> reply =
        base::JSONReader::ReadDict(message, base::JSON_PARSE_RFC);
    if (!reply) {
      return;
    }
    std::optional<int> id = reply->FindInt("id");
    if (!id) {
      // A CDP event (id-less): fan out to this session's subscribers as a
      // substrate subscription frame {event, params} (ACM-4). Events with
      // no producer are dropped — nothing subscribed.
      const std::string* method = reply->FindString("method");
      if (!method) {
        return;
      }
      auto producer_it = producers_.find(*method);
      if (producer_it == producers_.end()) {
        return;
      }
      const base::DictValue* params = reply->FindDict("params");
      producer_it->second->Broadcast(Value::make_object({
          {"event", Value(*method)},
          {"params", params ? FromBaseDict(*params) : Value()},
      }));
      return;
    }
    auto it = in_flight_.find(*id);
    if (it == in_flight_.end()) {
      return;
    }
    std::shared_ptr<PendingCdpAnswer> answer = std::move(it->second);
    in_flight_.erase(it);

    if (const base::DictValue* error = reply->FindDict("error")) {
      // The host's own refusal passes through, typed.
      std::optional<std::string> error_json = base::WriteJson(*error);
      answer->SettleBroken("cdp-error:" +
                           error_json.value_or("(unserializable)"));
    } else if (const base::DictValue* result = reply->FindDict("result")) {
      answer->SettleValue(FromBaseDict(*result));
    } else {
      answer->SettleValue(Value());
    }
    CompletionBridge::Get().NotifySettled(answer);
  }

  // Detach-on-target-close (design section 3): session teardown settles
  // every in-flight entry Broken. The browser target never closes; the
  // page-target RED rides ACM-3.
  void OnClosed() {
    auto in_flight = std::move(in_flight_);
    in_flight_.clear();
    for (auto& [id, answer] : in_flight) {
      answer->SettleBroken("cdp-session-closed");
      CompletionBridge::Get().NotifySettled(answer);
    }
    if (on_closed_external_) {
      std::move(on_closed_external_).Run();
    }
  }

  std::unique_ptr<CdpAgentClient> client_;
  int next_id_ = 1;
  std::map<int, std::shared_ptr<PendingCdpAnswer>> in_flight_;
  // ACM-4: per-event-method fan-out + the domains already enabled on this
  // session (the auto-enable is once per domain per session).
  std::map<std::string, std::unique_ptr<SubscriptionProducer>> producers_;
  std::set<std::string> enabled_domains_;
  base::OnceClosure on_closed_external_;
  base::WeakPtrFactory<CdpSession> weak_factory_{this};
};

struct CdpSessionRegistry::Impl {
  // The persistent browser-target session — lazily attached on the first
  // invoke (hosts are lightweight until attached; sessions attach lazily,
  // design section 2).
  std::unique_ptr<CdpSession> browser_session;
  // ACM-3: the persistent per-target sessions, keyed by the target id the
  // enumeration minted. Lazy attach per target; pruned on target close.
  std::map<std::string, std::unique_ptr<CdpSession>> target_sessions;
};

CdpSessionRegistry& CdpSessionRegistry::Get() {
  static base::NoDestructor<CdpSessionRegistry> registry;
  return *registry;
}

CdpSessionRegistry::CdpSessionRegistry() : impl_(std::make_unique<Impl>()) {}
CdpSessionRegistry::~CdpSessionRegistry() = default;

// The shared lazy-attach halves (invoke and subscribe ride the same
// persistent sessions). Null on attach failure.
CdpSession* CdpSessionRegistry::EnsureBrowserSession() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!impl_->browser_session || !impl_->browser_session->attached()) {
    auto session = std::make_unique<CdpSession>();
    if (!session->AttachToBrowserTarget()) {
      return nullptr;
    }
    impl_->browser_session = std::move(session);
  }
  return impl_->browser_session.get();
}

CdpSession* CdpSessionRegistry::EnsureTargetSession(
    const std::string& target_id) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  auto it = impl_->target_sessions.find(target_id);
  if (it == impl_->target_sessions.end() || !it->second->attached()) {
    auto session = std::make_unique<CdpSession>();
    if (!session->AttachToTarget(target_id)) {
      // A stale id (the target closed) is a clean attach failure.
      return nullptr;
    }
    // Detach-on-target-close (design section 3): OnClosed settles the
    // in-flight entries Broken synchronously, then this posts the prune —
    // the dead client is erased in a LATER UI turn, never mid-callback.
    session->set_on_closed(base::BindOnce([]() {
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE,
          base::BindOnce(&CdpSessionRegistry::PruneClosedTargetSessions,
                         base::Unretained(&CdpSessionRegistry::Get())));
    }));
    it = impl_->target_sessions.insert_or_assign(target_id,
                                                 std::move(session))
             .first;
  }
  return it->second.get();
}

std::shared_ptr<Handle> CdpSessionRegistry::InvokeOnBrowserTarget(
    const std::string& method,
    const Value& params) {
  CdpSession* session = EnsureBrowserSession();
  if (!session) {
    return ValueHandle::make_broken("cdp-attach-failed");
  }
  return session->Invoke(method, params);
}

std::shared_ptr<Handle> CdpSessionRegistry::InvokeOnTarget(
    const std::string& target_id,
    const std::string& method,
    const Value& params) {
  CdpSession* session = EnsureTargetSession(target_id);
  if (!session) {
    return ValueHandle::make_broken("cdp-attach-failed:no-such-target");
  }
  return session->Invoke(method, params);
}

std::shared_ptr<Handle> CdpSessionRegistry::SubscribeOnBrowserTarget(
    const std::string& event_method,
    std::shared_ptr<Handle> sink,
    const std::string& uri_prefix) {
  CdpSession* session = EnsureBrowserSession();
  if (!session) {
    return ValueHandle::make_broken("cdp-attach-failed");
  }
  return session->Subscribe(event_method, std::move(sink), uri_prefix);
}

std::shared_ptr<Handle> CdpSessionRegistry::SubscribeOnTarget(
    const std::string& target_id,
    const std::string& event_method,
    std::shared_ptr<Handle> sink,
    const std::string& uri_prefix) {
  CdpSession* session = EnsureTargetSession(target_id);
  if (!session) {
    return ValueHandle::make_broken("cdp-attach-failed:no-such-target");
  }
  return session->Subscribe(event_method, std::move(sink), uri_prefix);
}

void CdpSessionRegistry::PruneClosedTargetSessions() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  std::erase_if(impl_->target_sessions,
                [](const auto& entry) { return !entry.second->attached(); });
}

size_t CdpSessionRegistry::SessionCountForTesting() const {
  return impl_->browser_session && impl_->browser_session->attached() ? 1 : 0;
}

size_t CdpSessionRegistry::InFlightCountForTesting() const {
  size_t n = impl_->browser_session
                 ? impl_->browser_session->in_flight_count()
                 : 0;
  for (const auto& [id, session] : impl_->target_sessions) {
    n += session->in_flight_count();
  }
  return n;
}

size_t CdpSessionRegistry::TargetSessionCountForTesting() const {
  size_t n = 0;
  for (const auto& [id, session] : impl_->target_sessions) {
    if (session->attached()) {
      ++n;
    }
  }
  return n;
}

}  // namespace aurelian
