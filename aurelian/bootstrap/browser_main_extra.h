// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian — Velite's Chromium embodiment.
// C0: minimal browser-process bootstrap that creates the root ActorSpace
// and mounts legion://chrome/.

#ifndef AURELIAN_BOOTSTRAP_BROWSER_MAIN_EXTRA_H_
#define AURELIAN_BOOTSTRAP_BROWSER_MAIN_EXTRA_H_

#include "chrome/browser/chrome_browser_main_extra_parts.h"

#include <memory>

namespace aurelian {

// Opaque wrapper so the header doesn't leak Velite includes into Chrome.
struct ActorSpaceHolder;

class BrowserMainExtra : public ChromeBrowserMainExtraParts {
 public:
  BrowserMainExtra();
  ~BrowserMainExtra() override;

  BrowserMainExtra(const BrowserMainExtra&) = delete;
  BrowserMainExtra& operator=(const BrowserMainExtra&) = delete;

  // ChromeBrowserMainExtraParts:
  void PostCreateThreads() override;

 private:
  std::unique_ptr<ActorSpaceHolder> holder_;
};

}  // namespace aurelian

#endif  // AURELIAN_BOOTSTRAP_BROWSER_MAIN_EXTRA_H_
