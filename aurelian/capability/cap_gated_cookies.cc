// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/capability/cap_gated_cookies.h"

#include "aurelian/capability/cap_membrane.h"
#include "aurelian/handles/network/network_handle.h"

namespace aurelian {

std::string GatedSetCookie(content::BrowserContext* ctx,
                           const PubKey& anchor,
                           bool has_anchor,
                           const std::vector<CapLink>& chain,
                           const std::string& url,
                           const std::string& name,
                           const std::string& value,
                           int64_t now_unix) {
  // The browser membrane verifies the cap + enforces it on this write BEFORE
  // the subsystem call.
  std::string denied = CheckBrowserCap(chain, anchor, has_anchor, "cookies.set",
                                       url, now_unix);
  if (!denied.empty()) {
    return denied;
  }
  SetCookie(ctx, url, name, value);
  return std::string();
}

}  // namespace aurelian
