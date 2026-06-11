// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/mirror/cdp_agent_client.h"

#include <cstdint>
#include <utility>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "base/task/single_thread_task_runner.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/devtools_agent_host_client.h"

namespace aurelian {

namespace {

// One persistent client on one target: the BROWSER target when
// `target_id` is empty, otherwise the enumerated target the id names
// (page, tab, worker, … — ACM-3).
class AgentHostClient : public content::DevToolsAgentHostClient,
                        public CdpAgentClient {
 public:
  AgentHostClient(std::string target_id,
                  base::RepeatingCallback<void(const std::string&)> on_message,
                  base::OnceClosure on_closed)
      : target_id_(std::move(target_id)),
        on_message_(std::move(on_message)),
        on_closed_(std::move(on_closed)) {}

  ~AgentHostClient() override {
    if (attached_) {
      host_->DetachClient(this);
    }
  }

  // CdpAgentClient:
  bool Attach() override {
    if (target_id_.empty()) {
      host_ = content::DevToolsAgentHost::CreateForBrowser(
          /*tethering_task_runner=*/nullptr,
          content::DevToolsAgentHost::CreateServerSocketCallback());
    } else {
      // Hosts self-retain while their entity lives, so an id minted by the
      // targets enumeration resolves here; a stale id (closed target) is a
      // clean attach failure, never a crash.
      host_ = content::DevToolsAgentHost::GetForId(target_id_);
    }
    if (!host_) {
      return false;
    }
    attached_ = host_->AttachClient(this);
    return attached_;
  }

  bool attached() const override { return attached_; }

  void Send(const std::string& frame) override {
    std::vector<uint8_t> bytes(frame.begin(), frame.end());
    host_->DispatchProtocolMessage(this, bytes);
  }

  // content::DevToolsAgentHostClient (UI thread):
  void DispatchProtocolMessage(content::DevToolsAgentHost* /*host*/,
                               base::span<const uint8_t> message) override {
    on_message_.Run(std::string(message.begin(), message.end()));
  }

  void AgentHostClosed(content::DevToolsAgentHost* /*host*/) override {
    attached_ = false;
    if (on_closed_) {
      std::move(on_closed_).Run();
    }
  }

  // The mirror is an embedder-level surface, not untrusted web content.
  bool IsTrusted() override { return true; }

 private:
  const std::string target_id_;  // empty = the browser target
  base::RepeatingCallback<void(const std::string&)> on_message_;
  base::OnceClosure on_closed_;
  scoped_refptr<content::DevToolsAgentHost> host_;
  bool attached_ = false;
};

}  // namespace

// static
std::unique_ptr<CdpAgentClient> CdpAgentClient::CreateForBrowserTarget(
    base::RepeatingCallback<void(const std::string&)> on_message,
    base::OnceClosure on_closed) {
  return std::make_unique<AgentHostClient>(std::string(), std::move(on_message),
                                           std::move(on_closed));
}

// static
std::unique_ptr<CdpAgentClient> CdpAgentClient::CreateForTargetId(
    const std::string& target_id,
    base::RepeatingCallback<void(const std::string&)> on_message,
    base::OnceClosure on_closed) {
  return std::make_unique<AgentHostClient>(target_id, std::move(on_message),
                                           std::move(on_closed));
}

}  // namespace aurelian
