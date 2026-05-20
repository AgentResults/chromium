// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/asmodeus/permission_override.h"

#include <algorithm>

namespace asmodeus {

Permission ParsePermission(const std::string& name) {
  if (name == "microphone" || name == "mic") return Permission::kMicrophone;
  if (name == "camera" || name == "cam") return Permission::kCamera;
  if (name == "notifications") return Permission::kNotifications;
  if (name == "clipboard") return Permission::kClipboard;
  if (name == "geolocation" || name == "location") return Permission::kGeolocation;
  if (name == "midi") return Permission::kMidi;
  if (name == "screen" || name == "screencapture") return Permission::kScreenCapture;
  if (name == "all" || name == "*") return Permission::kAll;
  return Permission::kAll;
}

const char* PermissionToString(Permission perm) {
  switch (perm) {
    case Permission::kMicrophone: return "microphone";
    case Permission::kCamera: return "camera";
    case Permission::kNotifications: return "notifications";
    case Permission::kClipboard: return "clipboard";
    case Permission::kGeolocation: return "geolocation";
    case Permission::kMidi: return "midi";
    case Permission::kScreenCapture: return "screencapture";
    case Permission::kAll: return "all";
  }
  return "unknown";
}

PermissionOverride::PermissionOverride() = default;
PermissionOverride::~PermissionOverride() = default;

void PermissionOverride::SetAutoGrant(bool enabled) {
  auto_grant_ = enabled;
}

bool PermissionOverride::ShouldGrant(Permission perm) const {
  // Check explicit denials first
  if (std::find(denied_.begin(), denied_.end(), perm) != denied_.end()) {
    return false;
  }
  if (std::find(denied_.begin(), denied_.end(), Permission::kAll) !=
      denied_.end()) {
    return false;
  }

  // Check explicit grants
  if (std::find(granted_.begin(), granted_.end(), perm) != granted_.end()) {
    return true;
  }
  if (std::find(granted_.begin(), granted_.end(), Permission::kAll) !=
      granted_.end()) {
    return true;
  }

  // Fall back to auto-grant
  return auto_grant_;
}

void PermissionOverride::Grant(Permission perm) {
  // Remove from denied list if present
  denied_.erase(std::remove(denied_.begin(), denied_.end(), perm),
                denied_.end());
  // Add to granted list if not already there
  if (std::find(granted_.begin(), granted_.end(), perm) == granted_.end()) {
    granted_.push_back(perm);
  }
}

void PermissionOverride::Deny(Permission perm) {
  // Remove from granted list if present
  granted_.erase(std::remove(granted_.begin(), granted_.end(), perm),
                 granted_.end());
  // Add to denied list if not already there
  if (std::find(denied_.begin(), denied_.end(), perm) == denied_.end()) {
    denied_.push_back(perm);
  }
}

void PermissionOverride::Reset() {
  auto_grant_ = false;
  granted_.clear();
  denied_.clear();
}

}  // namespace asmodeus
