// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian: a browser-wide tab overview reachable from the root without holding
// a specific Browser* — walks BrowserList. (Per-Browser tab ops live in
// tabstrip_handle; this is the global snapshot the legion://chrome/tabs root
// node exposes.)

#ifndef AURELIAN_HANDLES_BROWSER_TABS_OVERVIEW_H_
#define AURELIAN_HANDLES_BROWSER_TABS_OVERVIEW_H_

#include <string>

namespace aurelian {

struct TabsOverview {
  // Total tabs open across every browser window.
  int open_count = 0;
  // Last-active browser's active-tab committed URL ("" if none). UI thread.
  std::string active_url;
};

// Snapshots the live tab strip across all browsers. UI thread.
TabsOverview GetTabsOverview();

}  // namespace aurelian

#endif  // AURELIAN_HANDLES_BROWSER_TABS_OVERVIEW_H_
