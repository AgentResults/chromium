// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/handles/browser/history_handle.h"

#include "base/functional/bind.h"
#include "base/run_loop.h"
#include "base/task/cancelable_task_tracker.h"
#include "components/history/core/browser/history_service.h"
#include "components/history/core/browser/history_types.h"
#include "components/history/core/browser/url_row.h"
#include "url/gurl.h"

namespace aurelian {

UrlVisitInfo QueryUrlHistory(history::HistoryService* service,
                             const std::string& url) {
  UrlVisitInfo info;
  if (!service) {
    return info;
  }
  base::RunLoop loop;
  base::CancelableTaskTracker tracker;
  service->QueryURL(
      GURL(url),
      base::BindOnce(
          [](UrlVisitInfo* out, base::RunLoop* l,
             history::QueryURLResult result) {
            out->visited = result.success;
            out->visit_count = result.success ? result.row.visit_count() : 0;
            l->Quit();
          },
          &info, &loop),
      &tracker);
  loop.Run();
  return info;
}

}  // namespace aurelian
