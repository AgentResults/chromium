// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/handles/browser/emulation_handle.h"

#include "content/public/browser/web_contents.h"
#include "third_party/blink/public/common/user_agent/user_agent_metadata.h"

namespace aurelian {

void SetUserAgentOverride(content::WebContents* wc, const std::string& ua) {
  if (!wc) {
    return;
  }
  blink::UserAgentOverride ua_override;
  ua_override.ua_string_override = ua;
  wc->SetUserAgentOverride(ua_override, /*override_in_new_tabs=*/true);
}

std::string GetUserAgentOverride(content::WebContents* wc) {
  if (!wc) {
    return std::string();
  }
  return wc->GetUserAgentOverride().ua_string_override;
}

}  // namespace aurelian
