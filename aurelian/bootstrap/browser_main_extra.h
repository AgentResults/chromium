// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AURELIAN_BOOTSTRAP_BROWSER_MAIN_EXTRA_H_
#define AURELIAN_BOOTSTRAP_BROWSER_MAIN_EXTRA_H_

#include "chrome/browser/chrome_browser_main_extra_parts.h"

#include <memory>

#include "aurelian/federation/uds_register.h"

namespace aurelian {

struct BrowserMainExtraImpl;

// HS-3/ACM-2w (design section 5, round-3/4/5 contract): the ONE
// ChromeDispatchFn, constructed once over the INSTALLED sealed root and
// exported from the bootstrap — both wire bring-ups (the production UDS
// register-in and the browsertest-only WSS scaffold) consume this same
// function; nothing else may mint a root from ambient authority. Before the
// one-shot install (or after teardown) the returned fn answers
// "broken:no-root" — fail-closed, never a second root.
ChromeDispatchFn InstalledChromeDispatch();

class BrowserMainExtra : public ChromeBrowserMainExtraParts {
 public:
  BrowserMainExtra();
  ~BrowserMainExtra() override;

  BrowserMainExtra(const BrowserMainExtra&) = delete;
  BrowserMainExtra& operator=(const BrowserMainExtra&) = delete;

  // ChromeBrowserMainExtraParts:
  void PostCreateThreads() override;
  void PreBrowserStart() override;
  void PostBrowserStart() override;
  void PostMainMessageLoopRun() override;

 private:
  std::unique_ptr<BrowserMainExtraImpl> impl_;
};

}  // namespace aurelian

#endif  // AURELIAN_BOOTSTRAP_BROWSER_MAIN_EXTRA_H_
