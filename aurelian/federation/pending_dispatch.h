// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// CF-6 (conformant-federation design §6.1) — F8b: the deferred-dispatch
// Handle. Wraps the UNCHANGED HS-1 CompletionRecord and maps its CAS states
// onto the substrate Handle lifecycle:
//
//   kPending   → Pending          (the dispatcher defers; the serve loop's
//                                  pump_pending_answers settles on
//                                  transition — promise pipelining drains
//                                  for free, dispatcher.hpp §5.1)
//   kCompleted → ResolvedValue    (WireReply::kValue — the canonical JSON
//                                  parsed back to the TYPED Value)
//              → Broken           (WireReply::kBroken — a refusal settles as
//                                  a refusal, carrying its reason)
//   kShutdown  → Broken(kDispatchShutdownReason)
//   deadline   → Broken(kDispatchTimeoutReason)  (checked in state_kind();
//                                  no timer thread — round-6: every record
//                                  is Stop()-flippable, nothing waits)
//
// The completed case reads the reply's KIND; it never sniffs the payload.
// A legitimate answer whose text begins "broken:" is an answer.
//
// Velite-typed INTERNAL header (the nav_handle_internal.h discipline).

#ifndef AURELIAN_FEDERATION_PENDING_DISPATCH_H_
#define AURELIAN_FEDERATION_PENDING_DISPATCH_H_

#include <memory>
#include <string>

#include "base/time/time.h"
#include "velite/agentspaces-wire/handle.hpp"

namespace aurelian {

struct CompletionRecord;

class PendingDispatchHandle : public velite::agentspaces::Handle {
 public:
  // `deadline` null (base::TimeTicks()) = no expiry; otherwise a pending
  // record observed past it reads Broken(kDispatchTimeoutReply). A
  // completion that already won the CAS outranks the deadline.
  static std::shared_ptr<PendingDispatchHandle> make(
      std::shared_ptr<CompletionRecord> record, base::TimeTicks deadline);

  velite::agentspaces::StateKind state_kind() const override;
  const velite::agentspaces::Value& resolved_value() const override;
  std::shared_ptr<velite::agentspaces::Handle> resolved_handle()
      const override;
  std::string_view broken_reason() const override;
  std::shared_ptr<velite::agentspaces::Handle> ask_impl(
      std::string_view msg, const velite::agentspaces::Value& spec) override;
  void tell(std::string_view msg,
            const velite::agentspaces::Value& data) override;

 private:
  PendingDispatchHandle(std::shared_ptr<CompletionRecord> record,
                        base::TimeTicks deadline);

  const std::shared_ptr<CompletionRecord> record_;
  const base::TimeTicks deadline_;
  // Renders the completed record's WireReply into value_ or broken_ —
  // once, on first observation (serve thread; state_kind/resolved_value are
  // pump-called there only).
  void Materialize() const;

  mutable velite::agentspaces::Value value_;
  mutable bool materialized_ = false;
  mutable bool broken_ = false;
  mutable std::string broken_reason_;
};

}  // namespace aurelian

#endif  // AURELIAN_FEDERATION_PENDING_DISPATCH_H_
