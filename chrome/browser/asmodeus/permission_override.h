// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ASMODEUS_PERMISSION_OVERRIDE_H_
#define CHROME_BROWSER_ASMODEUS_PERMISSION_OVERRIDE_H_

#include <string>
#include <vector>

namespace asmodeus {

// Known permission types that can be auto-granted.
enum class Permission {
  kMicrophone,
  kCamera,
  kNotifications,
  kClipboard,
  kGeolocation,
  kMidi,
  kScreenCapture,
  kAll,  // Grant everything
};

// Converts a permission name string to enum.
// Returns kAll for unknown strings (safe default for agent mode).
Permission ParsePermission(const std::string& name);

// Converts enum to string for logging.
const char* PermissionToString(Permission perm);

// Controls automatic permission granting for agent Chrome instances.
// When enabled, all permission requests are granted without showing
// any dialog to the user. This is essential for headless agents that
// need mic/camera access for meetings.
//
// Usage:
//   PermissionOverride override;
//   override.SetAutoGrant(true);
//   if (override.ShouldGrant(Permission::kMicrophone)) {
//     // Auto-grant without dialog
//   }
class PermissionOverride {
 public:
  PermissionOverride();
  ~PermissionOverride();

  // Enable/disable auto-grant for all permissions.
  void SetAutoGrant(bool enabled);

  // Check if a specific permission should be auto-granted.
  bool ShouldGrant(Permission perm) const;

  // Grant a specific permission (even if auto-grant is off).
  void Grant(Permission perm);

  // Deny a specific permission (even if auto-grant is on).
  void Deny(Permission perm);

  // Reset to default state (auto-grant off, no specific grants/denies).
  void Reset();

  bool auto_grant() const { return auto_grant_; }

 private:
  bool auto_grant_ = false;
  std::vector<Permission> granted_;
  std::vector<Permission> denied_;
};

}  // namespace asmodeus

#endif  // CHROME_BROWSER_ASMODEUS_PERMISSION_OVERRIDE_H_
