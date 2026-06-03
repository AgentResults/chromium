// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C4: network intercept + cookies + request observation + downloads.

#ifndef AURELIAN_HANDLES_NETWORK_NETWORK_HANDLE_H_
#define AURELIAN_HANDLES_NETWORK_NETWORK_HANDLE_H_

#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"
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

// --- Request observation (synchronous log, not streaming) ---
struct ObservedRequest {
  std::string url;
  std::string method;
};

std::vector<ObservedRequest> GetObservedRequests();
void ClearObservedRequests();

// --- Request observation as a stream (C6.e) ---
// Each request the throttle observes is delivered to every active subscriber's
// callback, on the sequence that called SubscribeRequests. Destroy the returned
// handle to cancel — delivery stops immediately (in-flight frames are dropped).
// SubscribeRequests + the returned handle's destruction MUST run on the same
// sequence (the delivery sequence).
using RequestFrameCallback =
    base::RepeatingCallback<void(const ObservedRequest&)>;

class RequestSubscription {
 public:
  virtual ~RequestSubscription() = default;
};

std::unique_ptr<RequestSubscription> SubscribeRequests(RequestFrameCallback cb);

// --- Download operations (UI thread) ---
struct DownloadInfo {
  uint32_t id = 0;
  std::string url;
  std::string state;  // "in_progress", "complete", "cancelled", "interrupted"
};

void StartDownload(content::BrowserContext* ctx, const std::string& url);
std::vector<DownloadInfo> GetDownloads(content::BrowserContext* ctx);

}  // namespace aurelian

#endif  // AURELIAN_HANDLES_NETWORK_NETWORK_HANDLE_H_
