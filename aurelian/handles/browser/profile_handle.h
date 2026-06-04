// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C13.d: profile info — incognito state + identity (/profiles seam).

#ifndef AURELIAN_HANDLES_BROWSER_PROFILE_HANDLE_H_
#define AURELIAN_HANDLES_BROWSER_PROFILE_HANDLE_H_

#include <string>

namespace content {
class BrowserContext;
}

namespace aurelian {

struct ProfileInfo {
  bool off_the_record = false;  // incognito / guest / other OTR
  std::string unique_id;
};

// Reads the profile (browser context) info. UI thread.
ProfileInfo GetProfileInfo(content::BrowserContext* ctx);

}  // namespace aurelian

#endif  // AURELIAN_HANDLES_BROWSER_PROFILE_HANDLE_H_
