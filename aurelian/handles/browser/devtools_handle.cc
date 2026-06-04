// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/handles/browser/devtools_handle.h"

#include <cstdint>
#include <vector>

#include "base/functional/callback.h"
#include "base/run_loop.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/devtools_agent_host_client.h"
#include "content/public/browser/web_contents.h"

namespace aurelian {

namespace {

// Transient client that attaches to a DevTools target, captures the response to
// our single request id, and quits the RunLoop when it arrives. CDP events have
// no "id"; only the command result carries "id":1.
class CdpClient : public content::DevToolsAgentHostClient {
 public:
  explicit CdpClient(base::OnceClosure on_response)
      : on_response_(std::move(on_response)) {}

  void DispatchProtocolMessage(content::DevToolsAgentHost* host,
                               base::span<const uint8_t> message) override {
    std::string m(message.begin(), message.end());
    if (m.find("\"id\":1") != std::string::npos) {
      response_ = m;
      if (on_response_) {
        std::move(on_response_).Run();
      }
    }
  }

  void AgentHostClosed(content::DevToolsAgentHost* host) override {}

  // The handle is an embedder-level surface, not untrusted web content.
  bool IsTrusted() override { return true; }

  const std::string& response() const { return response_; }

 private:
  base::OnceClosure on_response_;
  std::string response_;
};

// Captures the first received message whose text contains `needle` (used to
// match a CDP event by method name).
class CdpEventClient : public content::DevToolsAgentHostClient {
 public:
  CdpEventClient(std::string needle, base::OnceClosure on_event)
      : needle_(std::move(needle)), on_event_(std::move(on_event)) {}

  void DispatchProtocolMessage(content::DevToolsAgentHost* host,
                               base::span<const uint8_t> message) override {
    std::string m(message.begin(), message.end());
    if (event_.empty() && m.find(needle_) != std::string::npos) {
      event_ = m;
      if (on_event_) {
        std::move(on_event_).Run();
      }
    }
  }

  void AgentHostClosed(content::DevToolsAgentHost* host) override {}
  bool IsTrusted() override { return true; }

  const std::string& event() const { return event_; }

 private:
  std::string needle_;
  base::OnceClosure on_event_;
  std::string event_;
};

}  // namespace

std::string SendCdpCommand(content::WebContents* wc,
                           const std::string& method,
                           const std::string& params_json) {
  if (!wc) {
    return "{\"error\":\"no-web-contents\"}";
  }
  scoped_refptr<content::DevToolsAgentHost> host =
      content::DevToolsAgentHost::GetOrCreateFor(wc);
  if (!host) {
    return "{\"error\":\"no-agent-host\"}";
  }

  base::RunLoop run_loop;
  CdpClient client(run_loop.QuitClosure());
  if (!host->AttachClient(&client)) {
    return "{\"error\":\"attach-failed\"}";
  }

  std::string command = "{\"id\":1,\"method\":\"" + method +
                        "\",\"params\":" + params_json + "}";
  std::vector<uint8_t> bytes(command.begin(), command.end());
  host->DispatchProtocolMessage(&client, bytes);

  // Fallback timeout so a non-responding target can't hang the caller.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE, run_loop.QuitClosure(), base::Seconds(10));
  run_loop.Run();

  host->DetachClient(&client);
  if (client.response().empty()) {
    return "{\"error\":\"timeout\"}";
  }
  return client.response();
}

std::string CaptureCdpEvent(content::WebContents* wc,
                            const std::string& enable_method,
                            const std::string& action_method,
                            const std::string& action_params,
                            const std::string& event_method,
                            int timeout_ms) {
  if (!wc) {
    return "{\"error\":\"no-web-contents\"}";
  }
  scoped_refptr<content::DevToolsAgentHost> host =
      content::DevToolsAgentHost::GetOrCreateFor(wc);
  if (!host) {
    return "{\"error\":\"no-agent-host\"}";
  }

  base::RunLoop run_loop;
  CdpEventClient client(event_method, run_loop.QuitClosure());
  if (!host->AttachClient(&client)) {
    return "{\"error\":\"attach-failed\"}";
  }

  auto send = [&](int id, const std::string& method, const std::string& params) {
    std::string cmd = "{\"id\":" + std::to_string(id) + ",\"method\":\"" +
                      method + "\",\"params\":" + params + "}";
    std::vector<uint8_t> bytes(cmd.begin(), cmd.end());
    host->DispatchProtocolMessage(&client, bytes);
  };
  send(1, enable_method, "{}");
  send(2, action_method, action_params);

  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE, run_loop.QuitClosure(), base::Milliseconds(timeout_ms));
  run_loop.Run();

  host->DetachClient(&client);
  if (client.event().empty()) {
    return "{\"error\":\"timeout\"}";
  }
  return client.event();
}

}  // namespace aurelian
