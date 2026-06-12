// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_AUDIO_ASMODEUS_ASMODEUS_VIRTUAL_DEVICES_H_
#define MEDIA_AUDIO_ASMODEUS_ASMODEUS_VIRTUAL_DEVICES_H_

#include <string>

namespace asmodeus {

// Register a per-participant virtual audio output device.
// When Chrome creates an output stream with device_id "asmodeus-out-{name}",
// it will write to the shm file at shm_path instead of real speakers.
void RegisterVirtualOutputDevice(const std::string& name,
                                  const std::string& shm_path);
void UnregisterVirtualOutputDevice(const std::string& name);

}  // namespace asmodeus

#endif  // MEDIA_AUDIO_ASMODEUS_ASMODEUS_VIRTUAL_DEVICES_H_
