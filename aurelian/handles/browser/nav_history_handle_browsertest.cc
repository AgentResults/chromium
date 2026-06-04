// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C1.z browser tests — navigation history list.

#include "aurelian/handles/browser/nav_history_handle.h"

#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "url/gurl.h"

namespace aurelian {

class AurelianNavHistoryBrowserTest : public InProcessBrowserTest {
 protected:
  content::WebContents* GetWC() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }
};

IN_PROC_BROWSER_TEST_F(AurelianNavHistoryBrowserTest, ListsEntries) {
  const GURL a("data:text/html,<title>A</title>a");
  const GURL b("data:text/html,<title>B</title>b");
  const GURL c("data:text/html,<title>C</title>c");
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), a));
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), b));
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), c));

  content::WebContents* wc = GetWC();
  std::vector<HistoryEntry> history = GetNavigationHistory(wc);
  // The list starts with the initial (about:blank) entry, then A, B, C.
  ASSERT_GE(history.size(), 3u);
  const size_t n = history.size();
  EXPECT_EQ(history[n - 3].url, a.spec());
  EXPECT_EQ(history[n - 2].url, b.spec());
  EXPECT_EQ(history[n - 1].url, c.spec());
  EXPECT_EQ(CurrentHistoryIndex(wc), static_cast<int>(n) - 1);

  // Going back moves the committed index; the list is retained.
  wc->GetController().GoBack();
  ASSERT_TRUE(content::WaitForLoadStop(wc));
  EXPECT_EQ(CurrentHistoryIndex(wc), static_cast<int>(n) - 2);
  EXPECT_EQ(GetNavigationHistory(wc).size(), n);
}

}  // namespace aurelian
