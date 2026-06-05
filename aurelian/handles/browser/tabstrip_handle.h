// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C1.x: tabstrip mutation — open / close / activate (browser-UI
// handle). Completes the design §10 /browser/tabs open|close|activate verbs
// (C1 shipped list/read/navigate only).

#ifndef AURELIAN_HANDLES_BROWSER_TABSTRIP_HANDLE_H_
#define AURELIAN_HANDLES_BROWSER_TABSTRIP_HANDLE_H_

#include <string>

class Browser;

namespace aurelian {

// Opens a new tab at `url`. Returns its index, or -1 on failure. UI thread.
int OpenTab(Browser* browser, const std::string& url, bool foreground);

// Closes the tab at `index`. Returns false on a bad index or when it would
// close the browser's last tab (a window-close, out of scope here). UI thread.
bool CloseTab(Browser* browser, int index);

// Activates (foregrounds) the tab at `index`. Returns false on a bad index.
bool ActivateTab(Browser* browser, int index);

int TabCount(Browser* browser);
int ActiveTabIndex(Browser* browser);

// Browser-global convenience wrappers — each resolves the last-active Browser*
// (chrome::FindLastActive) internally, so the root's mounted `tabs` node can
// reach the interactive verbs without holding a specific Browser*. They return
// the same failure sentinels as the Browser*-taking forms when no browser is
// active (-1 / false / "" / 0). UI thread.
int OpenTabGlobal(const std::string& url, bool foreground);
bool ActivateTabGlobal(int index);
bool CloseTabGlobal(int index);
int TabCountGlobal();
int ActiveTabIndexGlobal();
// Last-committed URL of the tab at `index` ("" on a bad index / no browser).
std::string TabUrlGlobal(int index);

}  // namespace aurelian

#endif  // AURELIAN_HANDLES_BROWSER_TABSTRIP_HANDLE_H_
