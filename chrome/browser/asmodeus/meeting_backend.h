// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ASMODEUS_MEETING_BACKEND_H_
#define CHROME_BROWSER_ASMODEUS_MEETING_BACKEND_H_

#include <string>

namespace asmodeus {

// Abstract interface for connecting agents to a meeting platform.
// Implementations: NativeMeetingBackend (our own WebRTC meetings),
//                  GoogleMeetBackend (Google Meet via AsmodeusParticipant).
class MeetingBackend {
 public:
  virtual ~MeetingBackend() = default;

  // Start the backend for this meeting.
  virtual bool Start(const std::string& meeting_name) = 0;

  // Stop the backend and clean up.
  virtual void Stop() = 0;

  // Connect an agent to the meeting platform.
  // The agent process is already running with the given shm paths.
  virtual bool ConnectAgent(const std::string& agent_name,
                            const std::string& audio_shm_path,
                            const std::string& video_shm_path) = 0;

  // Disconnect an agent from the meeting platform.
  virtual void DisconnectAgent(const std::string& agent_name) = 0;

  // Get the signaling URL (for native meetings).
  // Returns empty string for backends that don't use signaling.
  virtual std::string GetSignalingUrl() const;
  virtual std::string GetSignalingHost() const;
  virtual int GetSignalingPort() const;

  virtual bool is_running() const = 0;
};

}  // namespace asmodeus

#endif  // CHROME_BROWSER_ASMODEUS_MEETING_BACKEND_H_
