// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/asmodeus/native_meeting_backend.h"

#include "base/logging.h"
#include "base/strings/stringprintf.h"
#include "chrome/browser/asmodeus/asmodeus_meeting_server.h"

namespace asmodeus {

NativeMeetingBackend::NativeMeetingBackend() = default;
NativeMeetingBackend::~NativeMeetingBackend() { Stop(); }

bool NativeMeetingBackend::Start(const std::string& meeting_name) {
  signaling_server_ = std::make_unique<AsmodeusMeetingServer>();

  // The signaling server needs HTML content for its HTTP endpoint.
  // For native meetings, no web pages are needed — agents are native binaries.
  // Pass a minimal placeholder page.
  std::string placeholder = "<!DOCTYPE html><html><body>Native meeting</body></html>";

  if (!signaling_server_->Start(meeting_name, placeholder)) {
    LOG(ERROR) << "Failed to start signaling server for meeting: " << meeting_name;
    signaling_server_.reset();
    return false;
  }

  LOG(INFO) << "Native meeting backend started: " << meeting_name
            << " signaling=ws://127.0.0.1:" << signaling_server_->port();
  return true;
}

void NativeMeetingBackend::Stop() {
  if (signaling_server_) {
    signaling_server_->Stop();
    signaling_server_.reset();
    LOG(INFO) << "Native meeting backend stopped";
  }
}

bool NativeMeetingBackend::ConnectAgent(const std::string& agent_name,
                                         const std::string&,
                                         const std::string&) {
  // No-op for native meetings. The agent connects to signaling on its own
  // via --signaling-host/port flags passed at spawn time.
  LOG(INFO) << "Native backend: agent " << agent_name << " connected (no-op)";
  return true;
}

void NativeMeetingBackend::DisconnectAgent(const std::string& agent_name) {
  // No-op. Agent disconnects from signaling when it shuts down.
  LOG(INFO) << "Native backend: agent " << agent_name << " disconnected (no-op)";
}

std::string NativeMeetingBackend::GetSignalingUrl() const {
  if (!signaling_server_ || !signaling_server_->is_running()) return "";
  return signaling_server_->GetSignalingUrl();
}

std::string NativeMeetingBackend::GetSignalingHost() const {
  return "127.0.0.1";
}

int NativeMeetingBackend::GetSignalingPort() const {
  if (!signaling_server_) return 0;
  return signaling_server_->port();
}

bool NativeMeetingBackend::is_running() const {
  return signaling_server_ && signaling_server_->is_running();
}

}  // namespace asmodeus
