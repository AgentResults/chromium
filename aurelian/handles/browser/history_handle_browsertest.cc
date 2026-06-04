// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C13.e browser tests — browsing history query.

#include "aurelian/handles/browser/history_handle.h"

#include "base/time/time.h"
#include "chrome/browser/history/history_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "components/history/core/browser/history_service.h"
#include "components/history/core/browser/history_types.h"
#include "components/history/core/test/history_service_test_util.h"
#include "components/keyed_service/core/service_access_type.h"
#include "content/public/test/browser_test.h"
#include "url/gurl.h"

namespace aurelian {

class AurelianHistoryBrowserTest : public InProcessBrowserTest {
 protected:
  history::HistoryService* Service() {
    return HistoryServiceFactory::GetForProfile(
        browser()->profile(), ServiceAccessType::EXPLICIT_ACCESS);
  }
};

IN_PROC_BROWSER_TEST_F(AurelianHistoryBrowserTest, QueryVisitedUrl) {
  history::HistoryService* service = Service();
  ASSERT_TRUE(service);

  const GURL url("http://aurelian.example.test/visited");
  service->AddPage(url, base::Time::Now(), history::SOURCE_BROWSED);
  history::BlockUntilHistoryProcessesPendingRequests(service);

  UrlVisitInfo info = QueryUrlHistory(service, url.spec());
  EXPECT_TRUE(info.visited);
  EXPECT_GE(info.visit_count, 1);

  // A never-visited URL.
  UrlVisitInfo none =
      QueryUrlHistory(service, "http://aurelian.example.test/never");
  EXPECT_FALSE(none.visited);
}

}  // namespace aurelian
