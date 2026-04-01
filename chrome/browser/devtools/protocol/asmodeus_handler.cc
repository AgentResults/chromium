// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/devtools/protocol/asmodeus_handler.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

#include "base/functional/bind.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "chrome/browser/asmodeus/asmodeus_state.h"
#include "chrome/browser/password_manager/profile_password_store_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "components/password_manager/core/browser/password_form.h"
#include "components/password_manager/core/browser/password_store/password_store_interface.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "crypto/hmac.h"
#include "crypto/hash.h"
#include "net/cookies/canonical_cookie.h"
#include "net/cookies/cookie_options.h"
#include "services/network/public/mojom/cookie_manager.mojom.h"
#include "url/gurl.h"

using String = protocol::String;
using DispatchResponse = protocol::DispatchResponse;

namespace {

struct IdPPattern {
  const char* url_contains;
  const char* provider;
  const char* flow_type;
};

constexpr IdPPattern kIdPPatterns[] = {
    {"accounts.google.com/o/oauth2", "google", "oauth"},
    {"accounts.google.com/signin", "google", "login"},
    {"github.com/login/oauth", "github", "oauth"},
    {"github.com/login", "github", "login"},
    {"login.microsoftonline.com", "microsoft", "oauth"},
    {".okta.com/oauth2", "okta", "oauth"},
    {".auth0.com/authorize", "auth0", "oauth"},
    {"appleid.apple.com/auth", "apple", "oauth"},
    {"/saml/SSO", nullptr, "saml"},
    {"/saml2/idp/SSOService", nullptr, "saml"},
    {"/adfs/ls/", "microsoft-adfs", "saml"},
};

constexpr char kStealthScript[] = R"(
(function() {
  Object.defineProperty(navigator, 'webdriver', {
    get: () => undefined, configurable: true
  });
  if (!window.chrome) window.chrome = {};
  if (!window.chrome.runtime) window.chrome.runtime = {};
  const origQuery = window.Permissions?.prototype?.query;
  if (origQuery) {
    window.Permissions.prototype.query = function(p) {
      if (p.name === 'notifications')
        return Promise.resolve({ state: Notification.permission });
      return origQuery.call(this, p);
    };
  }
})();
)";

}  // namespace

// ── PasswordStoreConsumer for async credential queries ──────────

class AsmodeusHandler::CredentialConsumer
    : public password_manager::PasswordStoreConsumer {
 public:
  using ResultCallback = base::OnceCallback<void(
      std::vector<std::unique_ptr<password_manager::PasswordForm>>)>;

  explicit CredentialConsumer(ResultCallback callback)
      : callback_(std::move(callback)) {}
  ~CredentialConsumer() override = default;

  base::WeakPtr<PasswordStoreConsumer> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

 protected:
  void OnGetPasswordStoreResults(
      std::vector<std::unique_ptr<password_manager::PasswordForm>> results)
      override {
    std::move(callback_).Run(std::move(results));
  }

 private:
  ResultCallback callback_;
  base::WeakPtrFactory<CredentialConsumer> weak_factory_{this};
};

// ── Constructor / Destructor ─────────────────────────────────────

AsmodeusHandler::ActiveFlow::ActiveFlow() = default;
AsmodeusHandler::ActiveFlow::ActiveFlow(const ActiveFlow&) = default;
AsmodeusHandler::ActiveFlow::ActiveFlow(ActiveFlow&&) = default;
AsmodeusHandler::ActiveFlow& AsmodeusHandler::ActiveFlow::operator=(const ActiveFlow&) = default;
AsmodeusHandler::ActiveFlow& AsmodeusHandler::ActiveFlow::operator=(ActiveFlow&&) = default;
AsmodeusHandler::ActiveFlow::~ActiveFlow() = default;

AsmodeusHandler::AsmodeusHandler(protocol::UberDispatcher* dispatcher,
                                 content::WebContents* web_contents)
    : web_contents_(web_contents) {
  protocol::Asmodeus::Dispatcher::wire(dispatcher, this);
  frontend_ =
      std::make_unique<protocol::Asmodeus::Frontend>(dispatcher->channel());
  if (web_contents_) {
    Observe(web_contents_);
  }
}

AsmodeusHandler::~AsmodeusHandler() = default;

// ── Enable / Disable ─────────────────────────────────────────────

DispatchResponse AsmodeusHandler::Enable() {
  enabled_ = true;
  return DispatchResponse::Success();
}

DispatchResponse AsmodeusHandler::Disable() {
  enabled_ = false;
  if (fingerprints_suppressed_) {
    asmodeus::SetSuppressed(false);
    fingerprints_suppressed_ = false;
  }
  active_flows_.clear();
  return DispatchResponse::Success();
}

// ── Fingerprint Suppression ──────────────────────────────────────

DispatchResponse AsmodeusHandler::SuppressFingerprints(bool in_suppress) {
  if (in_suppress == fingerprints_suppressed_) {
    return DispatchResponse::Success();
  }
  fingerprints_suppressed_ = in_suppress;
  asmodeus::SetSuppressed(in_suppress);
  if (in_suppress) {
    InjectStealthScript();
  }
  return DispatchResponse::Success();
}

void AsmodeusHandler::InjectStealthScript() {
  if (!web_contents_ || !web_contents_->GetPrimaryMainFrame()) {
    return;
  }
  web_contents_->GetPrimaryMainFrame()->ExecuteJavaScript(
      base::UTF8ToUTF16(std::string(kStealthScript)),
      base::NullCallback());
}

// ── Credential Management ────────────────────────────────────────

password_manager::PasswordStoreInterface* AsmodeusHandler::GetPasswordStore() {
  if (!web_contents_) return nullptr;
  Profile* profile =
      Profile::FromBrowserContext(web_contents_->GetBrowserContext());
  if (!profile) return nullptr;
  return ProfilePasswordStoreFactory::GetForProfile(
             profile, ServiceAccessType::EXPLICIT_ACCESS)
      .get();
}

void AsmodeusHandler::GetCredentials(
    const String& in_origin,
    std::unique_ptr<GetCredentialsCallback> callback) {
  auto* store = GetPasswordStore();
  if (!store) {
    callback->sendFailure(DispatchResponse::ServerError("Password store unavailable"));
    return;
  }

  GURL origin_url(in_origin);
  if (!origin_url.is_valid()) {
    callback->sendFailure(DispatchResponse::InvalidParams("Invalid origin"));
    return;
  }

  auto consumer = std::make_unique<CredentialConsumer>(base::BindOnce(
      [](std::unique_ptr<GetCredentialsCallback> cb, const std::string& filter,
         std::vector<std::unique_ptr<password_manager::PasswordForm>> forms) {
        auto creds = std::make_unique<
            protocol::Array<protocol::Asmodeus::Credential>>();
        for (const auto& form : forms) {
          if (form->url.spec().find(filter) != std::string::npos ||
              form->signon_realm.find(filter) != std::string::npos) {
            auto c = protocol::Asmodeus::Credential::Create()
                         .SetOrigin(form->url.spec())
                         .SetUsername(base::UTF16ToUTF8(form->username_value))
                         .SetPassword(base::UTF16ToUTF8(form->password_value))
                         .Build();
            creds->push_back(std::move(c));
          }
        }
        cb->sendSuccess(std::move(creds));
      },
      std::move(callback), in_origin));

  store->GetAllLoginsWithAffiliationAndBrandingInformation(
      consumer->GetWeakPtr());
  pending_consumers_.push_back(std::move(consumer));
}

void AsmodeusHandler::SaveCredentials(
    const String& in_origin, const String& in_username,
    const String& in_password, std::optional<String> in_totpSecret,
    std::optional<String> in_notes,
    std::unique_ptr<SaveCredentialsCallback> callback) {
  auto* store = GetPasswordStore();
  if (!store) {
    callback->sendFailure(DispatchResponse::ServerError("Password store unavailable"));
    return;
  }

  GURL origin_url(in_origin);
  if (!origin_url.is_valid()) {
    callback->sendFailure(DispatchResponse::InvalidParams("Invalid origin"));
    return;
  }

  password_manager::PasswordForm form;
  form.url = origin_url;
  form.signon_realm = origin_url.DeprecatedGetOriginAsURL().spec();
  form.username_value = base::UTF8ToUTF16(in_username);
  form.password_value = base::UTF8ToUTF16(in_password);
  form.date_created = base::Time::Now();
  form.date_last_used = base::Time::Now();

  store->AddLogin(form);
  callback->sendSuccess();
}

void AsmodeusHandler::DeleteCredentials(
    const String& in_origin, const String& in_username,
    std::unique_ptr<DeleteCredentialsCallback> callback) {
  auto* store = GetPasswordStore();
  if (!store) {
    callback->sendFailure(DispatchResponse::ServerError("Password store unavailable"));
    return;
  }

  password_manager::PasswordForm form;
  form.url = GURL(in_origin);
  form.signon_realm = GURL(in_origin).DeprecatedGetOriginAsURL().spec();
  form.username_value = base::UTF8ToUTF16(in_username);
  store->RemoveLogin(FROM_HERE, form);
  callback->sendSuccess();
}

void AsmodeusHandler::ListCredentials(
    std::unique_ptr<ListCredentialsCallback> callback) {
  auto* store = GetPasswordStore();
  if (!store) {
    callback->sendFailure(DispatchResponse::ServerError("Password store unavailable"));
    return;
  }

  auto consumer = std::make_unique<CredentialConsumer>(base::BindOnce(
      [](std::unique_ptr<ListCredentialsCallback> cb,
         std::vector<std::unique_ptr<password_manager::PasswordForm>> forms) {
        auto creds = std::make_unique<
            protocol::Array<protocol::Asmodeus::Credential>>();
        for (const auto& form : forms) {
          auto c = protocol::Asmodeus::Credential::Create()
                       .SetOrigin(form->url.spec())
                       .SetUsername(base::UTF16ToUTF8(form->username_value))
                       .SetPassword(base::UTF16ToUTF8(form->password_value))
                       .Build();
          creds->push_back(std::move(c));
        }
        cb->sendSuccess(std::move(creds));
      },
      std::move(callback)));

  store->GetAllLoginsWithAffiliationAndBrandingInformation(
      consumer->GetWeakPtr());
  pending_consumers_.push_back(std::move(consumer));
}

// ── TOTP Generation ──────────────────────────────────────────────

std::vector<uint8_t> AsmodeusHandler::Base32Decode(const std::string& input) {
  static constexpr std::string_view kBase32Chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
  std::vector<uint8_t> result;
  int buffer = 0, bits = 0;
  for (char c : input) {
    if (c == '=' || c == ' ') continue;
    c = base::ToUpperASCII(c);
    auto pos = kBase32Chars.find(c);
    if (pos == std::string::npos) continue;
    buffer = (buffer << 5) | static_cast<int>(pos);
    bits += 5;
    if (bits >= 8) {
      bits -= 8;
      result.push_back(static_cast<uint8_t>((buffer >> bits) & 0xFF));
    }
  }
  return result;
}

std::string AsmodeusHandler::ComputeTOTP(const std::string& base32_secret,
                                         int digits, int period,
                                         int64_t* remaining_seconds) {
  auto key = Base32Decode(base32_secret);
  if (key.empty()) return "";

  int64_t now = static_cast<int64_t>(
      base::Time::Now().InSecondsFSinceUnixEpoch());
  int64_t counter = now / period;
  if (remaining_seconds) *remaining_seconds = period - (now % period);

  std::array<uint8_t, 8> counter_bytes;
  int64_t tmp = counter;
  for (int i = 7; i >= 0; --i) {
    counter_bytes[static_cast<size_t>(i)] = static_cast<uint8_t>(tmp & 0xFF);
    tmp >>= 8;
  }

  auto hash = crypto::hmac::SignSha1(key, counter_bytes);

  int offset = hash[19] & 0x0F;
  int32_t code = ((hash[offset] & 0x7F) << 24) |
                 ((hash[offset + 1] & 0xFF) << 16) |
                 ((hash[offset + 2] & 0xFF) << 8) |
                 (hash[offset + 3] & 0xFF);

  int modulo = 1;
  for (int i = 0; i < digits; ++i) modulo *= 10;
  code %= modulo;

  std::string result = std::to_string(code);
  while (static_cast<int>(result.length()) < digits) result = "0" + result;
  return result;
}

DispatchResponse AsmodeusHandler::GenerateTOTP(
    const String& in_secret, std::optional<int> in_digits,
    std::optional<int> in_period, String* out_code,
    double* out_remainingSeconds) {
  int d = in_digits.value_or(6);
  int p = in_period.value_or(30);
  if (d < 4 || d > 10)
    return DispatchResponse::InvalidParams("digits must be 4-10");
  if (p < 10 || p > 120)
    return DispatchResponse::InvalidParams("period must be 10-120");

  int64_t remaining = 0;
  *out_code = ComputeTOTP(in_secret, d, p, &remaining);
  *out_remainingSeconds = static_cast<double>(remaining);

  if (out_code->empty())
    return DispatchResponse::ServerError("Invalid TOTP secret");
  return DispatchResponse::Success();
}

// ── Auth Flow Detection ──────────────────────────────────────────

std::optional<AsmodeusHandler::AuthProviderMatch>
AsmodeusHandler::MatchAuthProvider(const GURL& url) {
  std::string url_str = url.spec();
  for (const auto& p : kIdPPatterns) {
    if (url_str.find(p.url_contains) != std::string::npos) {
      return AuthProviderMatch{p.provider ? p.provider : "", p.flow_type};
    }
  }
  return std::nullopt;
}

void AsmodeusHandler::DidStartNavigation(
    content::NavigationHandle* navigation_handle) {
  if (!enabled_ || !navigation_handle->IsInPrimaryMainFrame()) return;
  auto match = MatchAuthProvider(navigation_handle->GetURL());
  if (!match) return;

  if (active_flows_.empty() && web_contents_) {
    pre_auth_origin_ =
        web_contents_->GetLastCommittedURL().DeprecatedGetOriginAsURL().spec();
  }

  ActiveFlow af;
  af.type = match->type;
  af.origin = pre_auth_origin_;
  af.state = "in-progress";
  af.provider = match->provider;
  active_flows_.push_back(std::move(af));

  if (frontend_) {
    auto proto_flow = protocol::Asmodeus::AuthFlow::Create()
                    .SetType(match->type)
                    .SetOrigin(pre_auth_origin_)
                    .SetState("detected")
                    .Build();
    if (!match->provider.empty()) proto_flow->SetProvider(match->provider);
    frontend_->AuthFlowDetected(std::move(proto_flow));
  }
}

void AsmodeusHandler::DidFinishNavigation(
    content::NavigationHandle* navigation_handle) {
  if (!enabled_ || active_flows_.empty() ||
      !navigation_handle->IsInPrimaryMainFrame() ||
      !navigation_handle->HasCommitted())
    return;

  std::string current =
      navigation_handle->GetURL().DeprecatedGetOriginAsURL().spec();

  if (!pre_auth_origin_.empty() && current == pre_auth_origin_) {
    if (frontend_) {
      for (const auto& f : active_flows_) {
        auto flow = protocol::Asmodeus::AuthFlow::Create()
                        .SetType(f.type)
                        .SetOrigin(f.origin)
                        .SetState("completed")
                        .Build();
        if (!f.provider.empty()) flow->SetProvider(f.provider);
        frontend_->AuthFlowCompleted(std::move(flow), true);
      }
    }
    active_flows_.clear();
    pre_auth_origin_.clear();
  }
}

DispatchResponse AsmodeusHandler::DetectAuthFlow(
    std::unique_ptr<protocol::Asmodeus::AuthFlow>* out_flow) {
  if (!web_contents_)
    return DispatchResponse::ServerError("No web contents");

  auto match = MatchAuthProvider(web_contents_->GetLastCommittedURL());
  if (match) {
    auto flow = protocol::Asmodeus::AuthFlow::Create()
                    .SetType(match->type)
                    .SetOrigin(web_contents_->GetLastCommittedURL()
                                   .DeprecatedGetOriginAsURL()
                                   .spec())
                    .SetState("detected")
                    .Build();
    if (!match->provider.empty()) flow->SetProvider(match->provider);
    *out_flow = std::move(flow);
  }
  return DispatchResponse::Success();
}

DispatchResponse AsmodeusHandler::GetAuthState(
    std::unique_ptr<protocol::Array<protocol::Asmodeus::AuthFlow>>*
        out_activeFlows) {
  auto flows =
      std::make_unique<protocol::Array<protocol::Asmodeus::AuthFlow>>();
  for (const auto& f : active_flows_) {
    auto flow = protocol::Asmodeus::AuthFlow::Create()
                    .SetType(f.type)
                    .SetOrigin(f.origin)
                    .SetState(f.state)
                    .Build();
    if (!f.provider.empty()) flow->SetProvider(f.provider);
    flows->push_back(std::move(flow));
  }
  *out_activeFlows = std::move(flows);
  return DispatchResponse::Success();
}

// ── Login Form Detection ─────────────────────────────────────────

DispatchResponse AsmodeusHandler::DetectLoginForm(
    std::unique_ptr<protocol::Asmodeus::LoginForm>* out_form) {
  // Form detection runs via Runtime.evaluate from the JS handle layer.
  // The C++ handler provides the CDP interface; the JS does DOM analysis.
  return DispatchResponse::Success();
}

void AsmodeusHandler::AutoLogin(
    const String& in_origin, std::optional<String> in_username,
    std::unique_ptr<AutoLoginCallback> callback) {
  // Orchestrated from the JS layer. C++ confirms credentials exist.
  auto* store = GetPasswordStore();
  if (!store) {
    callback->sendFailure(DispatchResponse::ServerError("Password store unavailable"));
    return;
  }

  auto consumer = std::make_unique<CredentialConsumer>(base::BindOnce(
      [](std::unique_ptr<AutoLoginCallback> cb, const std::string& origin,
         const std::string& username,
         std::vector<std::unique_ptr<password_manager::PasswordForm>> forms) {
        for (const auto& form : forms) {
          if (form->url.spec().find(origin) != std::string::npos ||
              form->signon_realm.find(origin) != std::string::npos) {
            if (username.empty() ||
                base::UTF16ToUTF8(form->username_value) == username) {
              cb->sendSuccess(true, std::optional<String>());
              return;
            }
          }
        }
        cb->sendSuccess(false, std::optional<String>(
                                   "No credentials for " + origin));
      },
      std::move(callback), std::string(in_origin),
      in_username.has_value() ? std::string(in_username.value())
                              : std::string()));

  store->GetAllLoginsWithAffiliationAndBrandingInformation(
      consumer->GetWeakPtr());
  pending_consumers_.push_back(std::move(consumer));
}

// ── Session Persistence ──────────────────────────────────────────

void AsmodeusHandler::ExportSession(
    std::optional<String> in_origin,
    std::unique_ptr<ExportSessionCallback> callback) {
  if (!web_contents_) {
    callback->sendFailure(DispatchResponse::ServerError("No web contents"));
    return;
  }

  auto* partition =
      web_contents_->GetBrowserContext()->GetDefaultStoragePartition();
  if (!partition) {
    callback->sendFailure(DispatchResponse::ServerError("No storage partition"));
    return;
  }

  auto* cookie_mgr = partition->GetCookieManagerForBrowserProcess();
  if (!cookie_mgr) {
    callback->sendFailure(DispatchResponse::ServerError("No cookie manager"));
    return;
  }

  std::string filter = in_origin.has_value() ? std::string(in_origin.value())
                                             : std::string();

  cookie_mgr->GetAllCookies(base::BindOnce(
      [](std::unique_ptr<ExportSessionCallback> cb, const std::string& filter,
         const std::vector<net::CanonicalCookie>& cookies) {
        auto sessions = std::make_unique<
            protocol::Array<protocol::Asmodeus::SessionSnapshot>>();

        std::map<std::string, std::vector<const net::CanonicalCookie*>>
            by_domain;
        for (const auto& c : cookies) {
          std::string d = c.Domain();
          if (d.starts_with(".")) d = d.substr(1);
          if (!filter.empty() && filter.find(d) == std::string::npos) continue;
          by_domain[d].push_back(&c);
        }

        for (const auto& [domain, domain_cookies] : by_domain) {
          auto arr = std::make_unique<
              protocol::Array<protocol::Asmodeus::Cookie>>();
          for (const auto* c : domain_cookies) {
            auto pc = protocol::Asmodeus::Cookie::Create()
                          .SetName(c->Name())
                          .SetValue(c->Value())
                          .SetDomain(c->Domain())
                          .SetPath(c->Path())
                          .SetExpires(c->ExpiryDate().InSecondsFSinceUnixEpoch())
                          .SetHttpOnly(c->IsHttpOnly())
                          .SetSecure(c->SecureAttribute())
                          .SetSession(!c->IsPersistent())
                          .Build();
            arr->push_back(std::move(pc));
          }
          auto snap = protocol::Asmodeus::SessionSnapshot::Create()
                          .SetOrigin("https://" + domain)
                          .SetCookies(std::move(arr))
                          .Build();
          snap->SetTimestamp(
              base::Time::Now().InSecondsFSinceUnixEpoch());
          sessions->push_back(std::move(snap));
        }
        cb->sendSuccess(std::move(sessions));
      },
      std::move(callback), filter));
}

void AsmodeusHandler::ImportSession(
    std::unique_ptr<protocol::Array<protocol::Asmodeus::SessionSnapshot>>
        in_sessions,
    std::unique_ptr<ImportSessionCallback> callback) {
  if (!web_contents_) {
    callback->sendFailure(DispatchResponse::ServerError("No web contents"));
    return;
  }

  auto* partition =
      web_contents_->GetBrowserContext()->GetDefaultStoragePartition();
  if (!partition) {
    callback->sendFailure(DispatchResponse::ServerError("No storage partition"));
    return;
  }

  auto* cookie_mgr = partition->GetCookieManagerForBrowserProcess();
  if (!cookie_mgr) {
    callback->sendFailure(DispatchResponse::ServerError("No cookie manager"));
    return;
  }

  for (const auto& snap : *in_sessions) {
    const auto* cookies = snap->GetCookies();
    if (!cookies) continue;
    for (const auto& c : *cookies) {
      auto canonical = net::CanonicalCookie::CreateSanitizedCookie(
          GURL(snap->GetOrigin()), c->GetName(), c->GetValue(),
          c->GetDomain(), c->GetPath(), base::Time(),
          base::Time::FromSecondsSinceUnixEpoch(c->GetExpires()),
          base::Time(), c->GetSecure(), c->GetHttpOnly(),
          net::CookieSameSite::LAX_MODE, net::CookiePriority::COOKIE_PRIORITY_MEDIUM,
          std::nullopt, nullptr);
      if (canonical) {
        net::CookieOptions opts;
        opts.set_include_httponly();
        opts.set_same_site_cookie_context(
            net::CookieOptions::SameSiteCookieContext::MakeInclusive());
        cookie_mgr->SetCanonicalCookie(*canonical, GURL(snap->GetOrigin()),
                                       opts, base::DoNothing());
      }
    }
  }
  callback->sendSuccess();
}
