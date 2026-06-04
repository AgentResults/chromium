// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C11.b browser tests — responding to a pending JS dialog.

#include "aurelian/handles/browser/dialogs_handle.h"

#include "base/run_loop.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/javascript_dialogs/tab_modal_dialog_manager.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "url/gurl.h"

namespace aurelian {

class AurelianDialogsBrowserTest : public InProcessBrowserTest {
 protected:
  content::WebContents* GetWC() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }
};

IN_PROC_BROWSER_TEST_F(AurelianDialogsBrowserTest, AcceptsConfirm) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>dlg</title>")));
  content::WebContents* wc = GetWC();

  auto* manager =
      javascript_dialogs::TabModalDialogManager::FromWebContents(wc);
  ASSERT_TRUE(manager);

  // Trigger a confirm() (blocks the renderer) and wait until it is shown.
  base::RunLoop shown;
  manager->SetDialogShownCallbackForTesting(shown.QuitClosure());
  content::ExecuteScriptAsync(
      wc, "window.__r = confirm('proceed?') ? 'yes' : 'no';");
  shown.Run();

  // Respond by accepting; the renderer's confirm() returns true. (ASSERT so a
  // RED run aborts here instead of hanging on the dependent EvalJs below while
  // the renderer is still blocked in confirm().)
  ASSERT_TRUE(RespondToDialog(wc, /*accept=*/true));

  EXPECT_EQ(content::EvalJs(wc,
                            "(async()=>{for(let i=0;i<200;i++){"
                            "if(window.__r)return window.__r;"
                            "await new Promise(r=>setTimeout(r,10));}"
                            "return 'timeout';})()")
                .ExtractString(),
            "yes");

  // No dialog is pending now, so a second response is a no-op.
  EXPECT_FALSE(RespondToDialog(wc, true));
}

}  // namespace aurelian
