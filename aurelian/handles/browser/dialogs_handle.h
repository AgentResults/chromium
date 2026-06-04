// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C11.b: JS dialogs — respond to a pending alert/confirm/prompt
// (/browser/tabs/<id>/dialogs seam).

#ifndef AURELIAN_HANDLES_BROWSER_DIALOGS_HANDLE_H_
#define AURELIAN_HANDLES_BROWSER_DIALOGS_HANDLE_H_

namespace content {
class WebContents;
}

namespace aurelian {

// Responds to the tab's currently-pending JavaScript dialog: `accept` clicks
// OK/confirm, false cancels/dismisses. Returns true if a dialog was handled,
// false if none was pending. UI thread.
bool RespondToDialog(content::WebContents* wc, bool accept);

}  // namespace aurelian

#endif  // AURELIAN_HANDLES_BROWSER_DIALOGS_HANDLE_H_
