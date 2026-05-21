// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C1 regression tests — tab handles via InProcessBrowserTest.
// Tests the public C API (CreateTabHandle etc.) without touching Velite
// internals, so no RTTI conflict with Chrome's no-RTTI compilation.

#include "aurelian/handles/browser/tab_handle.h"

#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace aurelian {

class AurelianTabHandleBrowserTest : public InProcessBrowserTest {};

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

}  // namespace aurelian
