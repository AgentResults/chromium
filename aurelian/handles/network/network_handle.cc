// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/handles/network/network_handle.h"

#include <mutex>

#include "base/logging.h"
#include "base/run_loop.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/storage_partition.h"
#include "net/base/net_errors.h"
#include "net/cookies/canonical_cookie.h"
#include "net/cookies/cookie_options.h"
#include "net/cookies/cookie_util.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/mojom/cookie_manager.mojom.h"
#include "url/gurl.h"

namespace aurelian {

InterceptRule::InterceptRule() = default;
InterceptRule::~InterceptRule() = default;
InterceptRule::InterceptRule(const InterceptRule&) = default;
InterceptRule& InterceptRule::operator=(const InterceptRule&) = default;
InterceptRule::InterceptRule(InterceptRule&&) = default;
InterceptRule& InterceptRule::operator=(InterceptRule&&) = default;

namespace {

// Thread-safe rule storage (accessed from IO thread throttle + UI thread API).
std::mutex& RuleMutex() {
  static auto* mu = new std::mutex();
  return *mu;
}

std::vector<InterceptRule>& RuleStore() {
  static auto* store = new std::vector<InterceptRule>();
  return *store;
}

int next_rule_id = 1;

// The Aurelian URLLoaderThrottle — applies intercept rules.
class AurelianURLThrottle : public blink::URLLoaderThrottle {
 public:
  AurelianURLThrottle() = default;
  ~AurelianURLThrottle() override = default;

  void WillStartRequest(network::ResourceRequest* request,
                        bool* defer) override {
    std::string url = request->url.spec();
    std::lock_guard<std::mutex> lock(RuleMutex());
    for (const auto& rule : RuleStore()) {
      if (url.find(rule.url_pattern) == std::string::npos) continue;

      if (rule.action == "block") {
        delegate_->CancelWithError(net::ERR_BLOCKED_BY_CLIENT);
        return;
      }
      if (rule.action == "redirect" && !rule.redirect_url.empty()) {
        request->url = GURL(rule.redirect_url);
        return;
      }
    }
    // Default pass-through — no rules matched.
  }
};

}  // namespace

// --- Rule API ---

int AddInterceptRule(InterceptRule rule) {
  std::lock_guard<std::mutex> lock(RuleMutex());
  rule.id = next_rule_id++;
  RuleStore().push_back(std::move(rule));
  return RuleStore().back().id;
}

bool RemoveInterceptRule(int rule_id) {
  std::lock_guard<std::mutex> lock(RuleMutex());
  auto& store = RuleStore();
  for (auto it = store.begin(); it != store.end(); ++it) {
    if (it->id == rule_id) {
      store.erase(it);
      return true;
    }
  }
  return false;
}

std::vector<InterceptRule> GetInterceptRules() {
  std::lock_guard<std::mutex> lock(RuleMutex());
  return RuleStore();
}

void ClearInterceptRules() {
  std::lock_guard<std::mutex> lock(RuleMutex());
  RuleStore().clear();
}

std::unique_ptr<blink::URLLoaderThrottle> CreateAurelianThrottle() {
  return std::make_unique<AurelianURLThrottle>();
}

// --- Cookie API (UI thread, synchronous via RunLoop) ---

CookieResult GetCookie(content::BrowserContext* ctx,
                       const std::string& url,
                       const std::string& name) {
  auto* partition = ctx->GetDefaultStoragePartition();
  auto* cookie_mgr = partition->GetCookieManagerForBrowserProcess();

  CookieResult result;
  base::RunLoop loop;
  cookie_mgr->GetCookieList(
      GURL(url), net::CookieOptions::MakeAllInclusive(),
      net::CookiePartitionKeyCollection(),
      base::BindOnce(
          [](CookieResult* out, const std::string& want_name,
             base::RunLoop* rl,
             const net::CookieAccessResultList& cookies,
             const net::CookieAccessResultList&) {
            for (const auto& cookie_with_access : cookies) {
              if (std::string(cookie_with_access.cookie.Name()) == want_name) {
                out->ok = true;
                out->value = cookie_with_access.cookie.Value();
                break;
              }
            }
            if (!out->ok) out->error = "not-found";
            rl->Quit();
          },
          &result, name, &loop));
  loop.Run();
  return result;
}

CookieResult SetCookie(content::BrowserContext* ctx,
                       const std::string& url,
                       const std::string& name,
                       const std::string& value) {
  auto* partition = ctx->GetDefaultStoragePartition();
  auto* cookie_mgr = partition->GetCookieManagerForBrowserProcess();

  GURL cookie_url(url);
  auto canonical = net::CanonicalCookie::CreateSanitizedCookie(
      cookie_url, name, value, std::string(cookie_url.host()), "/",
      base::Time(), base::Time(), base::Time(),
      cookie_url.SchemeIsCryptographic(),
      false,  // httponly
      net::CookieSameSite::LAX_MODE,
      net::COOKIE_PRIORITY_DEFAULT,
      std::nullopt,  // partition key
      /*status=*/nullptr);

  CookieResult result;
  if (!canonical) {
    result.error = "invalid-cookie";
    return result;
  }

  base::RunLoop loop;
  cookie_mgr->SetCanonicalCookie(
      *canonical, cookie_url,
      net::CookieOptions::MakeAllInclusive(),
      base::BindOnce(
          [](CookieResult* out, base::RunLoop* rl,
             net::CookieAccessResult access_result) {
            out->ok = access_result.status.IsInclude();
            if (!out->ok) out->error = "set-failed";
            rl->Quit();
          },
          &result, &loop));
  loop.Run();
  return result;
}

CookieResult DeleteCookie(content::BrowserContext* ctx,
                          const std::string& url,
                          const std::string& name) {
  auto* partition = ctx->GetDefaultStoragePartition();
  auto* cookie_mgr = partition->GetCookieManagerForBrowserProcess();

  auto filter = network::mojom::CookieDeletionFilter::New();
  filter->url = GURL(url);
  filter->cookie_name = name;

  CookieResult result;
  base::RunLoop loop;
  cookie_mgr->DeleteCookies(
      std::move(filter),
      base::BindOnce(
          [](CookieResult* out, base::RunLoop* rl, uint32_t count) {
            out->ok = count > 0;
            if (!out->ok) out->error = "not-found";
            rl->Quit();
          },
          &result, &loop));
  loop.Run();
  return result;
}

}  // namespace aurelian
