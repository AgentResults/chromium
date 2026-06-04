// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/handles/browser/profile_handle.h"

#include "content/public/browser/browser_context.h"

namespace aurelian {

ProfileInfo GetProfileInfo(content::BrowserContext* ctx) {
  ProfileInfo info;
  if (!ctx) {
    return info;
  }
  info.off_the_record = ctx->IsOffTheRecord();
  info.unique_id = ctx->UniqueId();
  return info;
}

}  // namespace aurelian
