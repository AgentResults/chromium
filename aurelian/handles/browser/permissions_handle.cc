// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/handles/browser/permissions_handle.h"

#include <optional>

#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/content_settings/core/common/content_settings.h"
#include "components/content_settings/core/common/content_settings_types.h"
#include "url/gurl.h"

namespace aurelian {

namespace {

// Maps a permission name to its content-settings type. nullopt = unrecognised.
std::optional<ContentSettingsType> TypeFor(const std::string& permission) {
  if (permission == "geolocation") return ContentSettingsType::GEOLOCATION;
  if (permission == "notifications") return ContentSettingsType::NOTIFICATIONS;
  if (permission == "camera") return ContentSettingsType::MEDIASTREAM_CAMERA;
  if (permission == "microphone") return ContentSettingsType::MEDIASTREAM_MIC;
  return std::nullopt;
}

bool SetPermission(HostContentSettingsMap* map,
                   const std::string& origin,
                   const std::string& permission,
                   ContentSetting setting) {
  std::optional<ContentSettingsType> type = TypeFor(permission);
  if (!map || !type) {
    return false;
  }
  GURL url(origin);
  map->SetContentSettingDefaultScope(url, url, *type, setting);
  return true;
}

}  // namespace

std::string QueryPermission(HostContentSettingsMap* map,
                            const std::string& origin,
                            const std::string& permission) {
  std::optional<ContentSettingsType> type = TypeFor(permission);
  if (!map || !type) {
    return "unknown-permission";
  }
  GURL url(origin);
  switch (map->GetContentSetting(url, url, *type)) {
    case CONTENT_SETTING_ALLOW:
      return "allow";
    case CONTENT_SETTING_BLOCK:
      return "block";
    case CONTENT_SETTING_ASK:
      return "ask";
    default:
      return "default";
  }
}

bool GrantPermission(HostContentSettingsMap* map,
                     const std::string& origin,
                     const std::string& permission) {
  return SetPermission(map, origin, permission, CONTENT_SETTING_ALLOW);
}

bool RevokePermission(HostContentSettingsMap* map,
                      const std::string& origin,
                      const std::string& permission) {
  return SetPermission(map, origin, permission, CONTENT_SETTING_BLOCK);
}

bool ResetPermission(HostContentSettingsMap* map,
                     const std::string& origin,
                     const std::string& permission) {
  return SetPermission(map, origin, permission, CONTENT_SETTING_DEFAULT);
}

}  // namespace aurelian
