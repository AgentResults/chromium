// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C13.e: browsing history — query whether a URL was visited.

#ifndef AURELIAN_HANDLES_BROWSER_HISTORY_HANDLE_H_
#define AURELIAN_HANDLES_BROWSER_HISTORY_HANDLE_H_

#include <string>

namespace history {
class HistoryService;
}

namespace aurelian {

struct UrlVisitInfo {
  bool visited = false;
  int visit_count = 0;
};

// Queries `service` for `url`'s visit info. Synchronous: spins a RunLoop on the
// async history backend. UI thread.
UrlVisitInfo QueryUrlHistory(history::HistoryService* service,
                             const std::string& url);

}  // namespace aurelian

#endif  // AURELIAN_HANDLES_BROWSER_HISTORY_HANDLE_H_
