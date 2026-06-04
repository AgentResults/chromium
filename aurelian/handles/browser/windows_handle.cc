// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/handles/browser/windows_handle.h"

#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "ui/gfx/geometry/rect.h"

namespace aurelian {

std::vector<WindowInfo> ListWindows(Profile* profile) {
  std::vector<WindowInfo> result;
  if (!profile) {
    return result;
  }
  for (Browser* browser : chrome::FindAllBrowsersWithProfile(profile)) {
    if (!browser) {
      continue;
    }
    WindowInfo info;
    info.tab_count = browser->tab_strip_model()->count();
    if (BrowserWindow* window = browser->window()) {
      gfx::Rect bounds = window->GetBounds();
      info.width = bounds.width();
      info.height = bounds.height();
      info.active = window->IsActive();
    }
    result.push_back(info);
  }
  return result;
}

}  // namespace aurelian
