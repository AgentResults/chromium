// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AURELIAN_BOOTSTRAP_BROWSER_MAIN_EXTRA_H_
#define AURELIAN_BOOTSTRAP_BROWSER_MAIN_EXTRA_H_

#include "chrome/browser/chrome_browser_main_extra_parts.h"

#include <memory>

namespace aurelian {

struct BrowserMainExtraImpl;

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
