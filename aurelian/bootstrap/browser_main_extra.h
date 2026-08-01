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

// HS-3/ACM-2w + CF-6 (F8b): the ONE exported dispatch root, BEGIN-form —
// constructed once over the INSTALLED sealed root; both wire bring-ups
// (the production UDS register-in and the conformance serve) consume this
// same function; nothing else may mint a root from ambient authority. It
// is record-creating and NON-BLOCKING (design §6.1): the returned
// CompletionRecord settles via the HS-1 session layer; before the one-shot
// install (or after teardown) it completes broken("no-root") — fail-closed,
// never a second root. The synchronous InstalledChromeDispatch retired
// with the blocking bridge (design §6.2, DELETE-default — no production
// consumer remained).
BeginChromeDispatchFn InstalledBeginChromeDispatch();

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
