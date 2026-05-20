// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/asmodeus/asmodeus_url_throttle.h"

#include "base/logging.h"
#include "net/base/net_errors.h"
#include "services/network/public/cpp/resource_request.h"

namespace asmodeus {

const void* const AsmodeusParticipantTag::kKey = &AsmodeusParticipantTag::kKey;

void AsmodeusURLThrottle::WillStartRequest(
    network::ResourceRequest* request, bool* defer) {
  // Let all requests through — reCAPTCHA must run to get a valid token.
}

std::unique_ptr<blink::URLLoaderThrottle> MaybeCreateAsmodeusThrottle(
    content::WebContents* wc) {
  if (AsmodeusParticipantTag::IsTagged(wc)) {
    return std::make_unique<AsmodeusURLThrottle>();
  }
  return nullptr;
}

}  // namespace asmodeus
