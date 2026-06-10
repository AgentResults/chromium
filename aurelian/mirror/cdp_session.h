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

#include "base/no_destructor.h"

namespace velite::agentspaces {
class Handle;
class Value;
}  // namespace velite::agentspaces

namespace aurelian {

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
      const velite::agentspaces::Value& params);

  // The lazy-attach observables (plan ACM-2: the first invoke attaches the
  // browser session; one persistent session carries concurrent commands).
  size_t SessionCountForTesting() const;
  size_t InFlightCountForTesting() const;

 private:
  friend class base::NoDestructor<CdpSessionRegistry>;

  CdpSessionRegistry();
  ~CdpSessionRegistry();

  struct Impl;  // hides the content:: client from this header
  std::unique_ptr<Impl> impl_;
};

}  // namespace aurelian

#endif  // AURELIAN_MIRROR_CDP_SESSION_H_
