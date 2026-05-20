// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ASMODEUS_NATIVE_MEETING_BACKEND_H_
#define CHROME_BROWSER_ASMODEUS_NATIVE_MEETING_BACKEND_H_

#include <memory>
#include <string>

#include "chrome/browser/asmodeus/meeting_backend.h"

namespace asmodeus {

class AsmodeusMeetingServer;

// Backend for our own native WebRTC meetings.
// Owns the signaling server. Agents connect via WebRTC on their own.
// ConnectAgent/DisconnectAgent are no-ops — the agent handles its own WebRTC.
class NativeMeetingBackend : public MeetingBackend {
 public:
  NativeMeetingBackend();
  ~NativeMeetingBackend() override;

  bool Start(const std::string& meeting_name) override;
  void Stop() override;
  bool ConnectAgent(const std::string& agent_name,
                    const std::string& audio_shm_path,
                    const std::string& video_shm_path) override;
  void DisconnectAgent(const std::string& agent_name) override;
  std::string GetSignalingUrl() const override;
  std::string GetSignalingHost() const override;
  int GetSignalingPort() const override;
  bool is_running() const override;

 private:
  std::unique_ptr<AsmodeusMeetingServer> signaling_server_;
};

}  // namespace asmodeus

#endif  // CHROME_BROWSER_ASMODEUS_NATIVE_MEETING_BACKEND_H_
