// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// ACM-2 (HS-1): the content-layer half of the persistent CDP session — a
// long-lived DevToolsAgentHostClient behind a plain API. Kept in a SEPARATE
// no-RTTI source_set for the same reason as find_glue: it subclasses
// content::DevToolsAgentHostClient, whose typeinfo is not emitted in
// Chromium's no-RTTI build, so subclassing it from the RTTI-compiled
// session TU would leave an undefined typeinfo at link time. The session
// layer talks to it through this velite-free interface.

#ifndef AURELIAN_MIRROR_CDP_AGENT_CLIENT_H_
#define AURELIAN_MIRROR_CDP_AGENT_CLIENT_H_

#include <memory>
#include <string>

#include "base/functional/callback.h"

namespace aurelian {

class CdpAgentClient {
 public:
  virtual ~CdpAgentClient() = default;

  // Attaches the persistent client to the BROWSER target (one host, one
  // client, until detach/close). False when the host refuses.
  virtual bool Attach() = 0;
  virtual bool attached() const = 0;

  // Sends one raw protocol frame on the attached session.
  virtual void Send(const std::string& frame) = 0;

  // `on_message` receives every protocol message the host delivers (UI
  // thread) — replies and events alike; correlation is the session layer's.
  // `on_closed` fires once if the target closes under us.
  static std::unique_ptr<CdpAgentClient> CreateForBrowserTarget(
      base::RepeatingCallback<void(const std::string&)> on_message,
      base::OnceClosure on_closed);
};

}  // namespace aurelian

#endif  // AURELIAN_MIRROR_CDP_AGENT_CLIENT_H_
