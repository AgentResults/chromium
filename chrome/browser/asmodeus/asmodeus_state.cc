// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/asmodeus/asmodeus_state.h"

#include <atomic>
#include <map>
#include <mutex>

#include "base/no_destructor.h"

namespace asmodeus {

namespace {
std::atomic<bool> g_suppressed{false};

// Device registry — mutex-protected because it's accessed from both
// the UI thread (CDP handler) and the audio thread (AudioManagerMac).
// Uses NoDestructor to satisfy Chromium's no-exit-time-destructor rule.
std::mutex& GetDeviceMutex() {
  static base::NoDestructor<std::mutex> mutex;
  return *mutex;
}

std::map<std::string, std::string>& GetDeviceMap() {
  static base::NoDestructor<std::map<std::string, std::string>> devices;
  return *devices;
}
}  // namespace

bool IsSuppressed() {
  return g_suppressed.load(std::memory_order_relaxed);
}

void SetSuppressed(bool suppressed) {
  g_suppressed.store(suppressed, std::memory_order_relaxed);
}

void RegisterVirtualDevice(const std::string& name,
                           const std::string& shm_path) {
  std::lock_guard<std::mutex> lock(GetDeviceMutex());
  GetDeviceMap()[name] = shm_path;
}

void UnregisterVirtualDevice(const std::string& name) {
  std::lock_guard<std::mutex> lock(GetDeviceMutex());
  GetDeviceMap().erase(name);
}

std::string GetVirtualDevicePath(const std::string& name) {
  std::lock_guard<std::mutex> lock(GetDeviceMutex());
  auto it = GetDeviceMap().find(name);
  if (it != GetDeviceMap().end()) {
    return it->second;
  }
  return std::string();
}

std::vector<std::pair<std::string, std::string>> GetVirtualDevices() {
  std::lock_guard<std::mutex> lock(GetDeviceMutex());
  return std::vector<std::pair<std::string, std::string>>(
      GetDeviceMap().begin(), GetDeviceMap().end());
}

bool HasVirtualDevices() {
  std::lock_guard<std::mutex> lock(GetDeviceMutex());
  return !GetDeviceMap().empty();
}

}  // namespace asmodeus
