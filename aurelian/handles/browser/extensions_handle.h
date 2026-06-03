// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C7.b: extensions — list / enable / disable.

#ifndef AURELIAN_HANDLES_BROWSER_EXTENSIONS_HANDLE_H_
#define AURELIAN_HANDLES_BROWSER_EXTENSIONS_HANDLE_H_

#include <string>
#include <vector>

namespace content {
class BrowserContext;
}

namespace aurelian {

struct ExtensionInfo {
  std::string id;
  std::string name;
  bool enabled = false;
};

// Lists every installed extension (enabled + disabled) in `ctx`. UI thread.
std::vector<ExtensionInfo> ListExtensions(content::BrowserContext* ctx);

// Enables / disables an installed extension by id. Returns false if no such
// extension is installed (or it cannot be toggled). UI thread.
bool EnableExtensionById(content::BrowserContext* ctx, const std::string& id);
bool DisableExtensionById(content::BrowserContext* ctx, const std::string& id);

}  // namespace aurelian

#endif  // AURELIAN_HANDLES_BROWSER_EXTENSIONS_HANDLE_H_
