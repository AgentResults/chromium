// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Global registry mapping RenderFrameHost IDs to AsmodeusParticipant names.
// Used by the audio routing layer to override output/input devices for
// participant WebContents. Thread-safe.
//
// This is the principled mechanism for per-participant audio isolation.
// No JavaScript injection needed — the browser process routes audio
// at the stream factory level based on frame ownership.

#ifndef MEDIA_AUDIO_ASMODEUS_ASMODEUS_PARTICIPANT_REGISTRY_H_
#define MEDIA_AUDIO_ASMODEUS_ASMODEUS_PARTICIPANT_REGISTRY_H_

#include <map>
#include <mutex>
#include <string>
#include <utility>

#include "base/no_destructor.h"

namespace asmodeus {

class ParticipantRegistry {
 public:
  static ParticipantRegistry& Get();

  // Register a frame as belonging to a participant.
  void Register(int process_id, int frame_id, const std::string& name);
  // Unregister all frames for a participant.
  void UnregisterByName(const std::string& name);
  // Look up participant name for a frame. Returns empty if not found.
  std::string Lookup(int process_id, int frame_id) const;
  // Get the virtual output device ID for a participant.
  // Returns "asmodeus-out-{name}" or empty if not a participant.
  std::string GetOutputDeviceId(int process_id, int frame_id) const;
  // Get the virtual input device ID for a participant.
  std::string GetInputDeviceId(int process_id, int frame_id) const;

  ParticipantRegistry();
  ~ParticipantRegistry();

 private:
  friend class base::NoDestructor<ParticipantRegistry>;
  mutable std::mutex mu_;
  std::map<std::pair<int, int>, std::string> frames_;  // (pid, fid) → name
};

}  // namespace asmodeus

#endif  // MEDIA_AUDIO_ASMODEUS_ASMODEUS_PARTICIPANT_REGISTRY_H_
