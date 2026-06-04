// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/handles/browser/extensions_handle.h"

#include "extensions/browser/disable_reason.h"
#include "extensions/browser/extension_registrar.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/common/extension.h"
#include "extensions/common/extension_set.h"

namespace aurelian {

std::vector<ExtensionInfo> ListExtensions(content::BrowserContext* ctx) {
  std::vector<ExtensionInfo> result;
  auto* registry = extensions::ExtensionRegistry::Get(ctx);
  if (!registry) {
    return result;
  }
  for (const auto& ext : registry->enabled_extensions()) {
    result.push_back({ext->id(), ext->name(), /*enabled=*/true});
  }
  for (const auto& ext : registry->disabled_extensions()) {
    result.push_back({ext->id(), ext->name(), /*enabled=*/false});
  }
  return result;
}

bool EnableExtensionById(content::BrowserContext* ctx, const std::string& id) {
  auto* registry = extensions::ExtensionRegistry::Get(ctx);
  auto* registrar = extensions::ExtensionRegistrar::Get(ctx);
  if (!registry || !registrar) {
    return false;
  }
  // Only an installed (enabled or disabled) extension can be enabled.
  if (!registry->GetInstalledExtension(id)) {
    return false;
  }
  registrar->EnableExtension(id);
  return true;
}

ExtensionDetails GetExtensionDetails(content::BrowserContext* ctx,
                                     const std::string& id) {
  ExtensionDetails details;
  auto* registry = extensions::ExtensionRegistry::Get(ctx);
  if (!registry) {
    return details;
  }
  const extensions::Extension* ext = registry->GetInstalledExtension(id);
  if (!ext) {
    return details;
  }
  details.found = true;
  details.id = ext->id();
  details.name = ext->name();
  details.version = ext->VersionString();
  details.description = ext->description();
  details.enabled = registry->enabled_extensions().Contains(id);
  return details;
}

bool DisableExtensionById(content::BrowserContext* ctx, const std::string& id) {
  auto* registry = extensions::ExtensionRegistry::Get(ctx);
  auto* registrar = extensions::ExtensionRegistrar::Get(ctx);
  if (!registry || !registrar) {
    return false;
  }
  if (!registry->GetInstalledExtension(id)) {
    return false;
  }
  registrar->DisableExtension(
      id, {extensions::disable_reason::DISABLE_USER_ACTION});
  return true;
}

}  // namespace aurelian
