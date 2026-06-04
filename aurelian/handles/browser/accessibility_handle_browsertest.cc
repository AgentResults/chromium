// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C7.a browser tests — accessibility tree reflects the page.

#include "aurelian/handles/browser/accessibility_handle.h"

#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace aurelian {

class AurelianA11yBrowserTest : public InProcessBrowserTest {
 protected:
  content::WebContents* GetWC() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }
};

IN_PROC_BROWSER_TEST_F(AurelianA11yBrowserTest, TreeReflectsPage) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(),
      GURL("data:text/html,<button>Hello A11y</button>"
           "<a href='https://example.com/'>a link</a>")));

  std::vector<AxNode> nodes = SnapshotAxTree(GetWC());
  ASSERT_FALSE(nodes.empty()) << "accessibility snapshot was empty";

  bool found_button = false;
  bool found_link = false;
  for (const auto& n : nodes) {
    if (n.role == "button" && n.name == "Hello A11y") found_button = true;
    if (n.role == "link" && n.name == "a link") found_link = true;
  }
  EXPECT_TRUE(found_button) << "expected a button node named 'Hello A11y'";
  EXPECT_TRUE(found_link) << "expected a link node named 'a link'";
}

IN_PROC_BROWSER_TEST_F(AurelianA11yBrowserTest, TreeJsonIsHierarchical) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<button>Hi A11y</button>")));

  std::string json = GetAxTreeJson(GetWC());
  // A nested tree (a root with a children array) carrying the button + name.
  EXPECT_NE(json.find("\"children\""), std::string::npos) << json;
  EXPECT_NE(json.find("\"role\":\"button\""), std::string::npos) << json;
  EXPECT_NE(json.find("Hi A11y"), std::string::npos) << json;
}

}  // namespace aurelian
