// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C7.d: devtools — a CDP-compat command surface.

#ifndef AURELIAN_HANDLES_BROWSER_DEVTOOLS_HANDLE_H_
#define AURELIAN_HANDLES_BROWSER_DEVTOOLS_HANDLE_H_

#include <string>

namespace content {
class WebContents;
}

namespace aurelian {

// Sends a single Chrome DevTools Protocol command to `wc`'s DevTools target and
// returns the JSON response string (the message carrying our request id).
// `params_json` is the CDP params object (default "{}"). Synchronous: attaches
// a transient client, spins a RunLoop until the response arrives (or a timeout),
// then detaches. UI thread.
//
// SURVIVES the HS-1 replacement as the RAW-SESSION LIVE-PIN HARNESS ONLY
// (ACM-3/4 note for the section-7 inventory): its remaining callers are the
// ACM-S2 session-context live pins, which arbitrate the derived table
// against the host WITHOUT the mirror in the loop — by design they cannot
// migrate onto the mirror. No production surface uses it. The CaptureCdpEvent
// one-shot was deleted at ACM-4 when its last caller migrated onto the
// subscribe surface.
std::string SendCdpCommand(content::WebContents* wc,
                           const std::string& method,
                           const std::string& params_json = "{}");

}  // namespace aurelian

#endif  // AURELIAN_HANDLES_BROWSER_DEVTOOLS_HANDLE_H_
