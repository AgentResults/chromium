// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C1.y: windows — list browser windows + basic state (/browser windows
// per design §10; C1 shipped /browser/tabs only).

#ifndef AURELIAN_HANDLES_BROWSER_WINDOWS_HANDLE_H_
#define AURELIAN_HANDLES_BROWSER_WINDOWS_HANDLE_H_

#include <vector>

class Profile;

namespace aurelian {

struct WindowInfo {
  int tab_count = 0;
  int width = 0;
  int height = 0;
  bool active = false;
};

// Lists the browser windows belonging to `profile`. UI thread.
std::vector<WindowInfo> ListWindows(Profile* profile);

}  // namespace aurelian

#endif  // AURELIAN_HANDLES_BROWSER_WINDOWS_HANDLE_H_
