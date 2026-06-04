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
std::string SendCdpCommand(content::WebContents* wc,
                           const std::string& method,
                           const std::string& params_json = "{}");

// Sends `enable_method` then `action_method`(`action_params`), and returns the
// first received CDP EVENT message whose text contains `event_method` (events
// have no request id). Returns "{"error":...}" on failure/timeout. Useful for
// capturing protocol events (e.g. Runtime.consoleAPICalled) rather than
// command responses.
std::string CaptureCdpEvent(content::WebContents* wc,
                            const std::string& enable_method,
                            const std::string& action_method,
                            const std::string& action_params,
                            const std::string& event_method,
                            int timeout_ms = 10000);

}  // namespace aurelian

#endif  // AURELIAN_HANDLES_BROWSER_DEVTOOLS_HANDLE_H_
