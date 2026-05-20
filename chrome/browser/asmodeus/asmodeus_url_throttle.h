// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ASMODEUS_ASMODEUS_URL_THROTTLE_H_
#define CHROME_BROWSER_ASMODEUS_ASMODEUS_URL_THROTTLE_H_

#include <set>

#include "base/supports_user_data.h"
#include "content/public/browser/web_contents.h"
#include "third_party/blink/public/common/loader/url_loader_throttle.h"

namespace asmodeus {

// Tag set on AsmodeusParticipant WebContents to identify them.
class AsmodeusParticipantTag : public base::SupportsUserData::Data {
 public:
  static const void* const kKey;
  static void Tag(content::WebContents* wc) {
    wc->SetUserData(kKey, std::make_unique<AsmodeusParticipantTag>());
  }
  static bool IsTagged(content::WebContents* wc) {
    return wc && wc->GetUserData(kKey) != nullptr;
  }
};

// Blocks reCAPTCHA requests for tagged Asmodeus participant WebContents.
class AsmodeusURLThrottle : public blink::URLLoaderThrottle {
 public:
  AsmodeusURLThrottle() = default;
  ~AsmodeusURLThrottle() override = default;

  void WillStartRequest(network::ResourceRequest* request,
                        bool* defer) override;
};

// Call from CreateURLLoaderThrottles to maybe add the throttle.
std::unique_ptr<blink::URLLoaderThrottle> MaybeCreateAsmodeusThrottle(
    content::WebContents* wc);

}  // namespace asmodeus

#endif  // CHROME_BROWSER_ASMODEUS_ASMODEUS_URL_THROTTLE_H_
