// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C8.g: a cap-gated browser op — the browser membrane (CheckBrowserCap)
// guards a REAL Chromium operation (a cookie write) end-to-end. Demonstrates
// that §11 enforcement actually protects a browser-process subsystem call, not
// just classifies verbs.

#ifndef AURELIAN_CAPABILITY_CAP_GATED_COOKIES_H_
#define AURELIAN_CAPABILITY_CAP_GATED_COOKIES_H_

#include <cstdint>
#include <string>
#include <vector>

#include "aurelian/capability/cap_chain.h"

namespace content {
class BrowserContext;
}

namespace aurelian {

// Sets a cookie ONLY if `chain` (verified back to `anchor`) permits the write.
// Returns "" if the cookie was set, otherwise the broken reason (and the cookie
// is NOT touched). UI thread.
std::string GatedSetCookie(content::BrowserContext* ctx,
                           const PubKey& anchor,
                           bool has_anchor,
                           const std::vector<CapLink>& chain,
                           const std::string& url,
                           const std::string& name,
                           const std::string& value,
                           int64_t now_unix);

}  // namespace aurelian

#endif  // AURELIAN_CAPABILITY_CAP_GATED_COOKIES_H_
