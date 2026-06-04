// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C1.z: navigation history — the back/forward entry list
// (/navigation entries; C1 shipped the entry count, not the list).

#ifndef AURELIAN_HANDLES_BROWSER_NAV_HISTORY_HANDLE_H_
#define AURELIAN_HANDLES_BROWSER_NAV_HISTORY_HANDLE_H_

#include <string>
#include <vector>

namespace content {
class WebContents;
}

namespace aurelian {

struct HistoryEntry {
  std::string url;
  std::string title;
};

// Lists the tab's navigation entries (oldest first). UI thread.
std::vector<HistoryEntry> GetNavigationHistory(content::WebContents* wc);

// The index of the currently-committed entry (-1 if none). UI thread.
int CurrentHistoryIndex(content::WebContents* wc);

}  // namespace aurelian

#endif  // AURELIAN_HANDLES_BROWSER_NAV_HISTORY_HANDLE_H_
