// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C2.x browser tests — frame tree listing.

#include "aurelian/handles/browser/frames_handle.h"

#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "url/gurl.h"

namespace aurelian {

class AurelianFramesBrowserTest : public InProcessBrowserTest {
 protected:
  content::WebContents* GetWC() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }
};

IN_PROC_BROWSER_TEST_F(AurelianFramesBrowserTest, ListsMainAndSubframes) {
  // A page with a subframe (srcdoc iframe).
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(),
      GURL("data:text/html,<title>f</title>"
           "<iframe srcdoc='<p>child</p>'></iframe>")));

  std::vector<FrameInfo> frames = ListFrames(GetWC());

  // Main frame + the subframe.
  ASSERT_GE(frames.size(), 2u);

  int main_frames = 0;
  bool has_subframe = false;
  for (const auto& f : frames) {
    if (f.is_main) ++main_frames;
    else has_subframe = true;
  }
  EXPECT_EQ(main_frames, 1);
  EXPECT_TRUE(has_subframe) << "expected at least one subframe";
}

}  // namespace aurelian
