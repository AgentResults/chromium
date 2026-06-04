// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C11.a: emulation — user-agent override (CDP Emulation domain seam,
// browser-side via WebContents).

#ifndef AURELIAN_HANDLES_BROWSER_EMULATION_HANDLE_H_
#define AURELIAN_HANDLES_BROWSER_EMULATION_HANDLE_H_

#include <string>

namespace content {
class WebContents;
}

namespace aurelian {

// Sets the tab's user-agent override (empty clears it). Subsequent navigations
// made with the override option active report this UA. UI thread.
void SetUserAgentOverride(content::WebContents* wc, const std::string& ua);

// Reads the tab's current user-agent override ("" if none). UI thread.
std::string GetUserAgentOverride(content::WebContents* wc);

}  // namespace aurelian

#endif  // AURELIAN_HANDLES_BROWSER_EMULATION_HANDLE_H_
