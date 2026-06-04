// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/handles/browser/dialogs_handle.h"

#include "components/javascript_dialogs/tab_modal_dialog_manager.h"
#include "content/public/browser/web_contents.h"

namespace aurelian {

bool RespondToDialog(content::WebContents* wc, bool accept) {
  if (!wc) {
    return false;
  }
  auto* manager =
      javascript_dialogs::TabModalDialogManager::FromWebContents(wc);
  if (!manager) {
    return false;
  }
  // HandleJavaScriptDialog responds to the pending dialog (if any) and returns
  // whether one was handled.
  return manager->HandleJavaScriptDialog(wc, accept,
                                         /*prompt_override=*/nullptr);
}

}  // namespace aurelian
