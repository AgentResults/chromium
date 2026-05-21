// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/handles/browser/tab_handle.h"

#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"
#include "velite/agentspaces-wire/handle.hpp"
#include "velite/agentspaces-wire/value_handle.hpp"

namespace aurelian {

using V = velite::agentspaces::Value;
using StateKind = velite::agentspaces::StateKind;

class AurelianTabHandleBrowserTest : public InProcessBrowserTest {
 protected:
  std::shared_ptr<velite::agentspaces::Handle>& GetHandle(
      TabHandleImpl* impl) {
    return *static_cast<std::shared_ptr<velite::agentspaces::Handle>*>(
        impl->handle_ptr);
  }
};

IN_PROC_BROWSER_TEST_F(AurelianTabHandleBrowserTest, CreateAndDestroy) {
  content::WebContents* wc =
      browser()->tab_strip_model()->GetActiveWebContents();
  ASSERT_NE(wc, nullptr);
  auto tab = CreateTabHandle(wc, 8001);
  ASSERT_NE(tab, nullptr);
  EXPECT_EQ(tab->tab_id, 8001);
  EXPECT_NE(tab->handle_ptr, nullptr);
  DestroyTabHandle(std::move(tab));
}

IN_PROC_BROWSER_TEST_F(AurelianTabHandleBrowserTest, TabsHandleCreates) {
  auto tabs = CreateTabsHandle();
  ASSERT_NE(tabs, nullptr);
  EXPECT_NE(tabs->handle_ptr, nullptr);
}

IN_PROC_BROWSER_TEST_F(AurelianTabHandleBrowserTest,
                        ClosedTabReturnsBrokenGone) {
  // Open a second tab so closing one doesn't close the browser.
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(),
      GURL("data:text/html,<html><body>tab1</body></html>")));
  ui_test_utils::NavigateToURLWithDisposition(
      browser(),
      GURL("data:text/html,<html><body>tab2</body></html>"),
      WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP);
  ASSERT_EQ(browser()->tab_strip_model()->count(), 2);

  // Create a handle for tab index 1 (the second tab).
  content::WebContents* wc =
      browser()->tab_strip_model()->GetWebContentsAt(1);
  ASSERT_NE(wc, nullptr);
  auto tab = CreateTabHandle(wc, 8099);
  auto& handle = GetHandle(tab.get());

  // Verify it works before closing.
  auto url_result = handle->ask("url", V());
  ASSERT_EQ(url_result->state_kind(), StateKind::ResolvedValue);

  // Close the tab (destroys the WebContents).
  browser()->tab_strip_model()->CloseWebContentsAt(
      1, TabCloseTypes::CLOSE_NONE);

  // Now the handle should return broken("gone") via WeakPtr nullification.
  auto after_close = handle->ask("url", V());
  ASSERT_EQ(after_close->state_kind(), StateKind::Broken);
  EXPECT_EQ(std::string(after_close->broken_reason()), "gone");

  DestroyTabHandle(std::move(tab));
}

}  // namespace aurelian
