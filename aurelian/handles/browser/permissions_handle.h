// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C7.c: permissions — query / grant / revoke (content settings).

#ifndef AURELIAN_HANDLES_BROWSER_PERMISSIONS_HANDLE_H_
#define AURELIAN_HANDLES_BROWSER_PERMISSIONS_HANDLE_H_

#include <string>

class HostContentSettingsMap;

namespace aurelian {

// Permission state for an origin: "allow" | "block" | "ask" | "default", or
// "unknown-permission" if the name isn't recognised. UI thread.
std::string QueryPermission(HostContentSettingsMap* map,
                            const std::string& origin,
                            const std::string& permission);

// Grants (ALLOW) / revokes (BLOCK) a permission for an origin. Returns false if
// the permission name isn't recognised. UI thread.
bool GrantPermission(HostContentSettingsMap* map,
                     const std::string& origin,
                     const std::string& permission);
bool RevokePermission(HostContentSettingsMap* map,
                      const std::string& origin,
                      const std::string& permission);

// Resets a permission for an origin back to its default (clears any
// grant/revoke). Returns false if the permission name isn't recognised.
bool ResetPermission(HostContentSettingsMap* map,
                     const std::string& origin,
                     const std::string& permission);

}  // namespace aurelian

#endif  // AURELIAN_HANDLES_BROWSER_PERMISSIONS_HANDLE_H_
