// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C5.e — zoom handle (legion://chrome/browser/tabs/<id>/zoom).
//
// RED-first: authored against the unbuilt AurelianZoomHandle.
// tell("setZoom", {level}) changes the tab's zoom; ask("level") reads it.

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

class AurelianZoomBrowserTest : public InProcessBrowserTest {
 protected:
  content::WebContents* GetWC() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }

  std::shared_ptr<velite::agentspaces::Handle>& GetHandle(
      TabHandleImpl* impl) {
    return *static_cast<std::shared_ptr<velite::agentspaces::Handle>*>(
        impl->handle_ptr);
  }

  double Level(velite::agentspaces::Handle* zoom) {
    auto r = zoom->ask("level", V());
    EXPECT_EQ(r->state_kind(), StateKind::ResolvedValue);
    return r->resolved_value().is_double() ? r->resolved_value().as_double()
                                           : -999.0;
  }
};

IN_PROC_BROWSER_TEST_F(AurelianZoomBrowserTest, ZoomHandleMounts) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<body>zoom</body>")));
  auto tab = CreateTabHandle(GetWC(), 9401);
  auto& handle = GetHandle(tab.get());

  auto zoom = handle->ask("zoom", V());
  ASSERT_EQ(zoom->state_kind(), StateKind::ResolvedValue);
  ASSERT_TRUE(zoom->resolved_value().is_string());
  EXPECT_NE(zoom->resolved_value().as_string().find(
                "legion://chrome/browser/tabs/9401/zoom"),
            std::string::npos);

  DestroyTabHandle(std::move(tab));
}

IN_PROC_BROWSER_TEST_F(AurelianZoomBrowserTest, SetZoomChangesLevel) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<body>zoom</body>")));
  auto tab = CreateTabHandle(GetWC(), 9402);
  auto& handle = GetHandle(tab.get());

  auto zoom = handle->ask("zoom", V());
  ASSERT_EQ(zoom->state_kind(), StateKind::ResolvedValue);

  zoom->tell("setZoom", V::make_object({{"level", V(2.0)}}));
  EXPECT_NEAR(Level(zoom.get()), 2.0, 0.01);

  DestroyTabHandle(std::move(tab));
}

IN_PROC_BROWSER_TEST_F(AurelianZoomBrowserTest, ResetZoomReturnsToDefault) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<body>zoom</body>")));
  auto tab = CreateTabHandle(GetWC(), 9403);
  auto& handle = GetHandle(tab.get());

  auto zoom = handle->ask("zoom", V());
  ASSERT_EQ(zoom->state_kind(), StateKind::ResolvedValue);

  zoom->tell("setZoom", V::make_object({{"level", V(3.0)}}));
  EXPECT_NEAR(Level(zoom.get()), 3.0, 0.01);

  zoom->tell("reset", V());
  EXPECT_NEAR(Level(zoom.get()), 0.0, 0.01) << "reset returns to default (0)";

  DestroyTabHandle(std::move(tab));
}

IN_PROC_BROWSER_TEST_F(AurelianZoomBrowserTest, PerTabZoomDoesNotAffectOthers) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<body>tab one</body>")));
  ui_test_utils::NavigateToURLWithDisposition(
      browser(), GURL("data:text/html,<body>tab two</body>"),
      WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP);
  ASSERT_EQ(browser()->tab_strip_model()->count(), 2);

  content::WebContents* wc0 = browser()->tab_strip_model()->GetWebContentsAt(0);
  content::WebContents* wc1 = browser()->tab_strip_model()->GetWebContentsAt(1);
  auto tab0 = CreateTabHandle(wc0, 9410);
  auto tab1 = CreateTabHandle(wc1, 9411);

  auto zoom0 = GetHandle(tab0.get())->ask("zoom", V());
  auto zoom1 = GetHandle(tab1.get())->ask("zoom", V());

  zoom0->tell("setZoom", V::make_object({{"level", V(2.5)}}));
  EXPECT_NEAR(Level(zoom0.get()), 2.5, 0.01);
  EXPECT_NEAR(Level(zoom1.get()), 0.0, 0.01)
      << "other tab's zoom must be unaffected";

  DestroyTabHandle(std::move(tab0));
  DestroyTabHandle(std::move(tab1));
}

}  // namespace aurelian
