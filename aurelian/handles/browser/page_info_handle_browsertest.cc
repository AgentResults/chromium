// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C11.c browser tests — page content info (MIME + encoding).

#include "aurelian/handles/browser/page_info_handle.h"

#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "url/gurl.h"

namespace aurelian {

class AurelianPageInfoBrowserTest : public InProcessBrowserTest {
 protected:
  content::WebContents* GetWC() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }
};

IN_PROC_BROWSER_TEST_F(AurelianPageInfoBrowserTest, ReportsMimeAndEncoding) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>p</title>body")));
  PageContentInfo html = GetPageContentInfo(GetWC());
  EXPECT_EQ(html.mime_type, "text/html");
  EXPECT_FALSE(html.encoding.empty());

  // A different content type is reflected.
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/plain,just text")));
  PageContentInfo plain = GetPageContentInfo(GetWC());
  EXPECT_EQ(plain.mime_type, "text/plain");
}

}  // namespace aurelian
