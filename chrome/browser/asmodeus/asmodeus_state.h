// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ASMODEUS_ASMODEUS_STATE_H_
#define CHROME_BROWSER_ASMODEUS_ASMODEUS_STATE_H_

#include <string>
#include <utility>
#include <vector>

// Global Asmodeus state flags. Checked by various Chrome subsystems
// to suppress browser UI when the agent is in control.
//
// This is intentionally a simple global rather than a service/singleton
// because it needs to be checked from hot paths (password manager,
// infobar creation, etc.) without dependency injection overhead.

namespace asmodeus {

// Returns true if Asmodeus fingerprint suppression is active.
// When true, browser UI elements that reveal automation should be hidden:
// - Password save/update prompts
// - Password breach warnings
// - Debugger infobar
// - "Save password?" bubble
bool IsSuppressed();
void SetSuppressed(bool suppressed);

// ── Virtual audio device registry ─────────────────────────────────
// Thread-safe registry of named virtual audio devices. Each device has
// a unique name and shm path. Used by AudioManagerMac to enumerate
// and create virtual microphone streams.

// Register a named virtual audio device with its shm input path.
void RegisterVirtualDevice(const std::string& name,
                           const std::string& shm_path);

// Unregister a named virtual audio device.
void UnregisterVirtualDevice(const std::string& name);

// Get the shm path for a named device. Returns empty if not found.
std::string GetVirtualDevicePath(const std::string& name);

// Get all registered virtual devices as (name, shm_path) pairs.
std::vector<std::pair<std::string, std::string>> GetVirtualDevices();

// Check if any virtual devices are registered.
bool HasVirtualDevices();

}  // namespace asmodeus

#endif  // CHROME_BROWSER_ASMODEUS_ASMODEUS_STATE_H_
