// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C4: network intercept + cookie handles.

#ifndef AURELIAN_HANDLES_NETWORK_NETWORK_HANDLE_H_
#define AURELIAN_HANDLES_NETWORK_NETWORK_HANDLE_H_

#include <memory>
#include <string>
#include <vector>

#include "third_party/blink/public/common/loader/url_loader_throttle.h"

namespace content {
class BrowserContext;
}

namespace aurelian {

// --- Intercept rules (thread-safe, accessed from IO + UI) ---
struct InterceptRule {
  InterceptRule();
  ~InterceptRule();
  InterceptRule(const InterceptRule&);
  InterceptRule& operator=(const InterceptRule&);
  InterceptRule(InterceptRule&&);
  InterceptRule& operator=(InterceptRule&&);

  int id = 0;
  std::string url_pattern;
  std::string action;
  std::string redirect_url;
  std::string mock_body;
  std::string mock_content_type;
};

// Global rule registry. Thread-safe (locked internally).
int AddInterceptRule(InterceptRule rule);
bool RemoveInterceptRule(int rule_id);
std::vector<InterceptRule> GetInterceptRules();
void ClearInterceptRules();

// Create a URLLoaderThrottle that applies intercept rules.
std::unique_ptr<blink::URLLoaderThrottle> CreateAurelianThrottle();

// Cookie operations (synchronous wrappers, must run on UI thread).
struct CookieResult {
  bool ok = false;
  std::string value;
  std::string error;
};

CookieResult GetCookie(content::BrowserContext* ctx,
                       const std::string& url,
                       const std::string& name);
CookieResult SetCookie(content::BrowserContext* ctx,
                       const std::string& url,
                       const std::string& name,
                       const std::string& value);
CookieResult DeleteCookie(content::BrowserContext* ctx,
                          const std::string& url,
                          const std::string& name);

}  // namespace aurelian

#endif  // AURELIAN_HANDLES_NETWORK_NETWORK_HANDLE_H_
