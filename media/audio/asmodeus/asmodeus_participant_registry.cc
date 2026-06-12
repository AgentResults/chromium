// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/audio/asmodeus/asmodeus_participant_registry.h"

#include "base/logging.h"
#include "base/no_destructor.h"

namespace asmodeus {

ParticipantRegistry::ParticipantRegistry() = default;
ParticipantRegistry::~ParticipantRegistry() = default;

ParticipantRegistry& ParticipantRegistry::Get() {
  static base::NoDestructor<ParticipantRegistry> instance;
  return *instance;
}

void ParticipantRegistry::Register(int process_id, int frame_id,
                                    const std::string& name) {
  std::lock_guard<std::mutex> lock(mu_);
  frames_[{process_id, frame_id}] = name;
  LOG(WARNING) << "[Asmodeus] Registry: frame (" << process_id << ","
               << frame_id << ") → participant '" << name << "'";
}

void ParticipantRegistry::UnregisterByName(const std::string& name) {
  std::lock_guard<std::mutex> lock(mu_);
  for (auto it = frames_.begin(); it != frames_.end();) {
    if (it->second == name) it = frames_.erase(it);
    else ++it;
  }
}

std::string ParticipantRegistry::Lookup(int process_id, int frame_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = frames_.find({process_id, frame_id});
  return (it != frames_.end()) ? it->second : "";
}

std::string ParticipantRegistry::GetOutputDeviceId(int process_id,
                                                     int frame_id) const {
  std::string name = Lookup(process_id, frame_id);
  return name.empty() ? "" : "asmodeus-out-" + name;
}

std::string ParticipantRegistry::GetInputDeviceId(int process_id,
                                                    int frame_id) const {
  std::string name = Lookup(process_id, frame_id);
  return name.empty() ? "" : "asmodeus-" + name;
}

}  // namespace asmodeus
