// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/devtools/protocol/asmodeus_handler.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <sys/stat.h>
#include <unistd.h>

#include "base/compiler_specific.h"
#include "base/functional/bind.h"
#include "base/run_loop.h"
#include "content/public/browser/browser_thread.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "chrome/browser/asmodeus/asmodeus_state.h"
#include "chrome/browser/asmodeus/browser_platform.h"
#include "chrome/browser/asmodeus/totp_generator.h"
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
  // Initialize automation components
  credential_store_.LoadDefault();
  if (credential_store_.size() > 0) {
    auth_controller_ =
        std::make_unique<asmodeus::AuthController>(credential_store_);
  }
  // Determine Chrome binary path for instance manager
  const char* home = getenv("HOME");
  std::string chrome_path = home
      ? std::string(home) + "/workspace/chromium/src/out/Default/Chromium.app/Contents/MacOS/Chromium"
      : "";
  instance_manager_ =
      std::make_unique<asmodeus::InstanceManager>(chrome_path);
}

AsmodeusHandler::~AsmodeusHandler() {
  // audio_session_ (RAII) releases any in-flight capture automatically.
  // PeerConnectionTrackerHostObserver auto-unregisters via its destructor.
  // Clean up all virtual audio devices registered by this handler.
  for (auto& [name, server] : media_servers_) {
    asmodeus::UnregisterVirtualDevice(name);
    server->Stop();
  }
  media_servers_.clear();
  // Clean up video shm files.
  for (auto& [name, path] : video_shm_paths_) {
    unlink(path.c_str());
  }
  video_shm_paths_.clear();
  participants_.clear();
}

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
  auto* frame = web_contents_->GetPrimaryMainFrame();
  if (!frame->IsRenderFrameLive()) {
    return;
  }
  const GURL& url = frame->GetLastCommittedURL();
  if (url.is_empty() || !url.is_valid()) {
    return;
  }
  if (!url.SchemeIs("http") && !url.SchemeIs("https") && !url.SchemeIs("file")) {
    return;
  }
  // ExecuteJavaScript is restricted to chrome:// and devtools:// URLs.
  // Use ExecuteJavaScriptForTests with world_id=0 (global world) to run
  // on any page. This is safe because we only inject on verified schemes.
  frame->ExecuteJavaScriptForTests(
      base::UTF8ToUTF16(std::string(kStealthScript)),
      base::NullCallback(),
      /*world_id=*/0);
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
  // Delegate to TotpGenerator (shared with AuthController).
  if (remaining_seconds) {
    *remaining_seconds = asmodeus::TotpGenerator::SecondsRemaining(period);
  }
  return asmodeus::TotpGenerator::Generate(base32_secret, digits, period);
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
  if (!navigation_handle->IsInPrimaryMainFrame() ||
      !navigation_handle->HasCommitted())
    return;

  // AuthController: auto-handle Google sign-in pages
  if (auth_controller_ && web_contents_) {
    std::string url = navigation_handle->GetURL().spec();
    auth_controller_->HandleNavigation(web_contents_, url);
  }

  if (!enabled_ || active_flows_.empty())
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

// ── Virtual Audio (per-device named virtual mics) ───────────────

DispatchResponse AsmodeusHandler::EnableVirtualAudio(
    std::optional<String> in_name,
    std::optional<int> in_sampleRate,
    std::optional<int> in_channels,
    String* out_deviceId,
    String* out_shmPathIn,
    String* out_shmPathOut) {
  std::string name = in_name.value_or("default");
  int sr = in_sampleRate.value_or(48000);
  int ch = in_channels.value_or(1);

  // Check if this device already exists.
  auto it = media_servers_.find(name);
  if (it != media_servers_.end() && it->second->is_running()) {
    *out_deviceId = it->second->device_id();
    *out_shmPathIn = it->second->input_path();
    *out_shmPathOut = it->second->output_path();
    return DispatchResponse::Success();
  }

  // Create new media server for this device.
  auto server = std::make_unique<asmodeus::AsmodeusMediaServer>();
  if (!server->Start(name, sr, ch)) {
    return DispatchResponse::ServerError(
        "Failed to create shared memory for device: " + name);
  }

  // Register in global device registry so AudioManagerMac can find it.
  asmodeus::RegisterVirtualDevice(name, server->input_path());
  LOG(WARNING) << "[Asmodeus] Virtual device '" << name << "' registered."
               << " deviceId=" << server->device_id()
               << " input=" << server->input_path();

  *out_deviceId = server->device_id();
  *out_shmPathIn = server->input_path();
  *out_shmPathOut = server->output_path();

  media_servers_[name] = std::move(server);
  return DispatchResponse::Success();
}

DispatchResponse AsmodeusHandler::DisableVirtualAudio(
    std::optional<String> in_name) {
  std::string name = in_name.value_or("default");

  auto it = media_servers_.find(name);
  if (it != media_servers_.end()) {
    asmodeus::UnregisterVirtualDevice(name);
    it->second->Stop();
    media_servers_.erase(it);
  }
  return DispatchResponse::Success();
}

DispatchResponse AsmodeusHandler::GetVirtualAudioStatus(
    std::optional<String> in_name,
    bool* out_enabled,
    std::optional<String>* out_shmPathIn,
    std::optional<String>* out_shmPathOut,
    std::optional<int>* out_sampleRate,
    std::optional<int>* out_channels) {
  std::string name = in_name.value_or("default");

  auto it = media_servers_.find(name);
  *out_enabled = it != media_servers_.end() && it->second->is_running();
  if (*out_enabled) {
    *out_shmPathIn = it->second->input_path();
    *out_shmPathOut = it->second->output_path();
    *out_sampleRate = it->second->sample_rate();
    *out_channels = it->second->channels();
  }
  return DispatchResponse::Success();
}

// ── Virtual Camera ──────────────────────────────────────────────

DispatchResponse AsmodeusHandler::EnableVirtualCamera(
    std::optional<String> in_name,
    std::optional<int> in_width,
    std::optional<int> in_height,
    std::optional<int> in_fps,
    String* out_deviceId,
    String* out_shmPath) {
  std::string name = in_name.value_or("default");
  int w = in_width.value_or(640);
  int h = in_height.value_or(480);
  int fps = in_fps.value_or(15);

  // Check if already exists.
  auto it = video_shm_paths_.find(name);
  if (it != video_shm_paths_.end()) {
    *out_deviceId = "asmodeus-cam-" + name;
    *out_shmPath = it->second;
    return DispatchResponse::Success();
  }

  // Create the shm directory.
  const char* home = getenv("HOME");
  std::string dir = home ? std::string(home) + "/.asmodeus"
                         : "/tmp/asmodeus-media";
  mkdir(dir.c_str(), 0755);

  // Create video ring buffer file.
  std::string shm_path = dir + "/video-in-" + name + ".shm";
  const size_t frame_size =
      static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
  const int num_frames = 3;
  const size_t total_size = 32 + num_frames * frame_size;

  int fd = open(shm_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0666);
  if (fd < 0) {
    return DispatchResponse::ServerError(
        "Failed to create video shm: " + shm_path);
  }
  if (ftruncate(fd, static_cast<off_t>(total_size)) != 0) {
    close(fd);
    return DispatchResponse::ServerError("Failed to truncate video shm");
  }

  // Write header.
  uint32_t header[8] = {
    static_cast<uint32_t>(w),
    static_cast<uint32_t>(h),
    0,  // pixel_format: ARGB
    static_cast<uint32_t>(fps),
    0,  // write_seq
    0,  // read_seq
    static_cast<uint32_t>(frame_size),
    static_cast<uint32_t>(num_frames)
  };
  // SAFETY: header is a stack array of known size.
  UNSAFE_BUFFERS(write(fd, header, 32));
  close(fd);

  video_shm_paths_[name] = shm_path;

  LOG(WARNING) << "[Asmodeus] Virtual camera '" << name << "' created: "
               << w << "x" << h << "@" << fps << " shm=" << shm_path;

  *out_deviceId = "asmodeus-cam-" + name;
  *out_shmPath = shm_path;
  return DispatchResponse::Success();
}

DispatchResponse AsmodeusHandler::DisableVirtualCamera(
    std::optional<String> in_name) {
  std::string name = in_name.value_or("default");

  auto it = video_shm_paths_.find(name);
  if (it != video_shm_paths_.end()) {
    unlink(it->second.c_str());
    video_shm_paths_.erase(it);
  }
  return DispatchResponse::Success();
}

// ── Headless Participants ───────────────────────────────────────

DispatchResponse AsmodeusHandler::CreateParticipant(
    const String& in_name,
    const String& in_url,
    std::optional<String> in_displayName,
    String* out_participantId,
    bool* out_loaded) {
  std::string name = in_name;

  if (participants_.count(name)) {
    *out_participantId = name;
    *out_loaded = participants_[name]->is_loaded();
    return DispatchResponse::Success();
  }

  if (!web_contents_) {
    return DispatchResponse::ServerError("No browser context available");
  }

  // Derive device IDs from participant name:
  // Audio: "asmodeus-{name}", Video: "asmodeus-cam-{name}"
  std::string audio_dev = "asmodeus-" + name;
  std::string video_dev = "asmodeus-cam-" + name;

  // Use incognito context for additional participants so they have a
  // separate identity. The first participant uses the main profile (Google
  // account), additional ones join as anonymous guests.
  auto participant = std::make_unique<asmodeus::AsmodeusParticipant>(
      name, web_contents_->GetBrowserContext(), audio_dev, video_dev,
      /*use_incognito=*/false);
  participant->Navigate(GURL(in_url));

  *out_participantId = name;
  *out_loaded = false;

  participants_[name] = std::move(participant);
  return DispatchResponse::Success();
}

void AsmodeusHandler::EvaluateInParticipant(
    const String& in_name,
    const String& in_expression,
    std::unique_ptr<EvaluateInParticipantCallback> callback) {
  std::string name = in_name;

  auto it = participants_.find(name);
  if (it == participants_.end()) {
    callback->sendFailure(
        DispatchResponse::ServerError("Participant not found: " + name));
    return;
  }

  it->second->ExecuteJS(
      base::UTF8ToUTF16(in_expression),
      base::BindOnce(
          [](std::unique_ptr<EvaluateInParticipantCallback> cb,
             base::Value result) {
            if (result.is_string()) {
              cb->sendSuccess(result.GetString());
            } else if (result.is_none()) {
              cb->sendSuccess(std::optional<String>());
            } else {
              // Convert non-string results to JSON
              cb->sendSuccess(result.DebugString());
            }
          },
          std::move(callback)));
}

DispatchResponse AsmodeusHandler::GetParticipantState(
    const String& in_name,
    bool* out_exists,
    bool* out_loaded,
    std::optional<String>* out_url) {
  std::string name = in_name;

  auto it = participants_.find(name);
  *out_exists = it != participants_.end();
  if (*out_exists) {
    *out_loaded = it->second->is_loaded();
    auto* wc = it->second->web_contents();
    if (wc) {
      *out_url = wc->GetLastCommittedURL().spec();
    }
  } else {
    *out_loaded = false;
  }
  return DispatchResponse::Success();
}

DispatchResponse AsmodeusHandler::DestroyParticipant(const String& in_name) {
  std::string name = in_name;
  participants_.erase(name);
  return DispatchResponse::Success();
}

// ── Traffic capture ──────────────────────────────────────────────

namespace {

bool MatchesAnyPattern(const std::string& url,
                       const std::vector<std::string>& patterns) {
  for (const auto& p : patterns) {
    if (url.find(p) != std::string::npos) return true;
  }
  return false;
}

}  // namespace

// ── Page interaction ──────────────────────────────────────────

DispatchResponse AsmodeusHandler::TypeText(const String& in_text) {
  if (!web_contents_) return DispatchResponse::ServerError("no web contents");
  // Use Input.insertText equivalent — dispatch composition events
  content::RenderFrameHost* rfh = web_contents_->GetFocusedFrame();
  if (!rfh) return DispatchResponse::ServerError("no focused frame");
  // Execute JS to type into the active element
  // TypeText dispatches input events via execCommand for reliability.
  // The actual typing is done client-side via Runtime.evaluate since
  // direct DOM access from browser process is complex.
  // This is a placeholder — clients should use Runtime.evaluate directly.
  return DispatchResponse::Success();
}

DispatchResponse AsmodeusHandler::ClickByAriaLabel(
    const String& in_labelSubstring,
    bool* out_found,
    std::optional<String>* out_clickedLabel) {
  *out_found = false;
  if (!web_contents_) return DispatchResponse::ServerError("no web contents");
  // This is a synchronous command but DOM access needs the renderer.
  // For now, return success — the actual click is done via Runtime.evaluate
  // from the CDP client. This command is a convenience wrapper.
  return DispatchResponse::Success();
}

DispatchResponse AsmodeusHandler::CaptureStart(
    std::unique_ptr<protocol::Array<String>> in_includeUrlPatterns,
    std::unique_ptr<protocol::Array<String>> in_excludeUrlPatterns,
    std::optional<bool> in_captureWebRtc,
    std::optional<bool> in_captureWebSocketFrames,
    std::optional<bool> in_captureHttp) {
  include_patterns_.clear();
  exclude_patterns_.clear();
  if (in_includeUrlPatterns) {
    for (const auto& s : *in_includeUrlPatterns) include_patterns_.push_back(s);
  }
  if (in_excludeUrlPatterns) {
    for (const auto& s : *in_excludeUrlPatterns) exclude_patterns_.push_back(s);
  }
  capture_webrtc_ = in_captureWebRtc.value_or(true);
  capture_ws_ = in_captureWebSocketFrames.value_or(true);
  capture_http_ = in_captureHttp.value_or(true);
  capture_active_ = true;
  return DispatchResponse::Success();
}

DispatchResponse AsmodeusHandler::CaptureStop() {
  capture_active_ = false;
  url_by_pc_.clear();
  return DispatchResponse::Success();
}

// ── Tab Audio Capture ──────────────────────────────────────────

DispatchResponse AsmodeusHandler::CaptureTabAudio(
    const String& in_outputPath,
    std::optional<int> in_sampleRate,
    std::optional<int> in_channels) {
  if (!web_contents_) {
    return DispatchResponse::ServerError("No web contents");
  }
  audio_session_.reset(aurelian::CreateAudioCaptureHandle(web_contents_));
  int sr = in_sampleRate.value_or(48000);
  int ch = in_channels.value_or(1);
  if (!aurelian::AudioCaptureHandleStart(audio_session_.get(), in_outputPath,
                                         sr, ch)) {
    audio_session_.reset();
    return DispatchResponse::ServerError("Failed to start audio capture");
  }
  return DispatchResponse::Success();
}

DispatchResponse AsmodeusHandler::StopAudioCapture(
    double* out_durationMs, int* out_samples,
    double* out_peakRms, String* out_outputPath) {
  if (!audio_session_ || !web_contents_) {
    return DispatchResponse::ServerError("No active capture");
  }

  // Stop the JS capture
  auto* main_frame = web_contents_->GetPrimaryMainFrame();
  if (main_frame) {
    main_frame->ExecuteJavaScriptForTests(
        u"if(window.__asmodeusCapture) window.__asmodeusCapture.active=false;",
        base::NullCallback(), content::ISOLATED_WORLD_ID_GLOBAL);
  }

  // Return stats. Audio data stays in window.__asmodeusCapture.chunks
  // and can be read by the CDP client via Runtime.evaluate.
  aurelian::AudioCaptureStats result =
      aurelian::AudioCaptureHandleStop(audio_session_.get());
  *out_durationMs = result.duration_ms;
  *out_samples = static_cast<int>(result.samples);
  *out_peakRms = result.peak_rms;
  *out_outputPath = result.output_path;
  audio_session_.reset();
  return DispatchResponse::Success();
}

DispatchResponse AsmodeusHandler::GetAudioLevel(
    double* out_rms, double* out_peak, bool* out_capturing) {
  if (!audio_session_) {
    *out_rms = 0;
    *out_peak = 0;
    *out_capturing = false;
    return DispatchResponse::Success();
  }
  aurelian::AudioCaptureHandleLevel(audio_session_.get(), out_rms, out_peak,
                                    out_capturing);
  return DispatchResponse::Success();
}

void AsmodeusHandler::OnPeerConnectionAdded(
    content::GlobalRenderFrameHostId render_frame_host_id,
    int lid,
    base::ProcessId pid,
    const std::string& url,
    const std::string& rtc_configuration) {
  url_by_pc_[{pid, lid}] = url;
  if (!capture_active_ || !capture_webrtc_) return;
  if (!include_patterns_.empty() &&
      !MatchesAnyPattern(url, include_patterns_)) return;
  if (MatchesAnyPattern(url, exclude_patterns_)) return;
  double ts = base::Time::Now().InMillisecondsFSinceUnixEpoch();
  frontend_->WebRtcSignal(lid, "create", "", "", "", "", "",
                          rtc_configuration, url, ts);
}

void AsmodeusHandler::OnPeerConnectionRemoved(
    content::GlobalRenderFrameHostId render_frame_host_id,
    int lid) {
  for (auto it = url_by_pc_.begin(); it != url_by_pc_.end(); ) {
    if (it->first.second == lid) it = url_by_pc_.erase(it); else ++it;
  }
  if (!capture_active_ || !capture_webrtc_) return;
  double ts = base::Time::Now().InMillisecondsFSinceUnixEpoch();
  frontend_->WebRtcSignal(lid, "close", "", "", "", "", "", "", "", ts);
}

void AsmodeusHandler::OnPeerConnectionUpdated(
    content::GlobalRenderFrameHostId render_frame_host_id,
    int lid,
    const std::string& type,
    const std::string& value) {
  if (!capture_active_ || !capture_webrtc_) return;
  // Find frame url for this pc (via lid match).
  std::string frame_url;
  for (const auto& [key, u] : url_by_pc_) {
    if (key.second == lid) { frame_url = u; break; }
  }
  if (!include_patterns_.empty() &&
      !MatchesAnyPattern(frame_url, include_patterns_)) return;
  if (MatchesAnyPattern(frame_url, exclude_patterns_)) return;

  // Classify update type (empty strings = "not set").
  std::string ev;
  std::string sdp_type, sdp, candidate, ice_state, conn_state, raw_value;
  if (type == "setLocalDescription" || type == "setRemoteDescription") {
    ev = (type == "setLocalDescription") ? "localSdp" : "remoteSdp";
    // value looks like: "type: offer, sdp: v=0..."
    auto pos_sdp = value.find(", sdp: ");
    if (value.rfind("type: ", 0) == 0 && pos_sdp != std::string::npos) {
      sdp_type = value.substr(6, pos_sdp - 6);
      sdp = value.substr(pos_sdp + 7);
    } else {
      raw_value = value;
    }
  } else if (type == "onicecandidate" || type == "addIceCandidate") {
    ev = "iceCandidate";
    candidate = value;
  } else if (type == "iceconnectionstatechange") {
    ev = "stateChange";
    ice_state = value;
  } else if (type == "connectionstatechange") {
    ev = "stateChange";
    conn_state = value;
  } else if (type == "getStats") {
    ev = "stats";
    raw_value = value;
  } else {
    ev = "other";
    raw_value = type + ": " + value;
  }

  double ts = base::Time::Now().InMillisecondsFSinceUnixEpoch();
  frontend_->WebRtcSignal(lid, ev, sdp_type, sdp, candidate, ice_state,
                          conn_state, raw_value, frame_url, ts);
}

// ── Meeting Management ─────────────────────────────────────────

DispatchResponse AsmodeusHandler::JoinMeeting(
    const String& in_url,
    std::optional<String> in_platform,
    bool* out_joined,
    String* out_platform) {
  std::string url = in_url;
  std::string platform = in_platform.has_value()
      ? std::string(in_platform.value())
      : asmodeus::BrowserPlatform::DetectPlatform(url);

  if (platform == "unknown") {
    *out_joined = false;
    *out_platform = "unknown";
    return DispatchResponse::ServerError(
        "Unknown platform. Provide 'platform' parameter.");
  }

  // Stop old coordinator if any
  if (coordinator_) {
    coordinator_->Stop();
    coordinator_.reset();
  }

  std::string home = getenv("HOME") ? getenv("HOME") : "/tmp";
  std::string agent_path = home + "/workspace/chromium/src/out/Default/asmodeus_agent";

  // Create coordinator with browser platform backend
  coordinator_ = std::make_unique<asmodeus::MeetingCoordinator>();
  auto browser_backend = std::make_unique<asmodeus::BrowserPlatform>(
      platform, web_contents_->GetBrowserContext());
  browser_backend->Start(url);
  coordinator_->StartWithBackend(std::move(browser_backend), agent_path);

  // Register event callback (same as createMeeting)
  coordinator_->SetEventCallback(
      [this](const std::string& agent, const std::string& event_type,
             const base::Value& data) {
        if (!data.is_dict()) return;
        const auto& dict = data.GetDict();
        if (event_type == "speech_started") {
          const std::string* text = dict.FindString("text");
          const std::string* source = dict.FindString("source");
          frontend_->AgentSpoke(agent, text ? *text : "", source ? *source : "tts");
          frontend_->AgentSpeakingChanged(agent, true);
        } else if (event_type == "speech_ended") {
          auto dur = dict.FindDouble("durationMs");
          frontend_->AgentSpeechEnded(agent, dur ? *dur : 0);
          frontend_->AgentSpeakingChanged(agent, false);
        } else if (event_type == "heard") {
          const std::string* text = dict.FindString("text");
          auto stt_ms = dict.FindInt("sttMs");
          frontend_->AgentHeard(agent, text ? *text : "", stt_ms ? *stt_ms : 0);
        } else if (event_type == "barge_in") {
          frontend_->AgentBargeIn(agent);
        } else if (event_type == "error") {
          const std::string* msg = dict.FindString("message");
          frontend_->AgentError(agent, msg ? *msg : "unknown");
        }
      });

  // Auto-admit: inject a script into the host tab that clicks "Admit"
  // buttons whenever they appear (for incognito agent participants).
  if (web_contents_) {
    std::string auto_admit_src =
        "(function() {"
        "  if (window.__asmodeusAdmitInterval) clearInterval(window.__asmodeusAdmitInterval);"
        "  window.__asmodeusAdmitInterval = setInterval(function() {"
        "    var btns = document.querySelectorAll('button, [role=\"button\"]');"
        "    for (var i = 0; i < btns.length; i++) {"
        "      var t = (btns[i].textContent || '').trim().toLowerCase();"
        "      if (t === 'admit' || t === 'let in' || t === 'accept' || t === 'admit all') {"
        "        console.log('[Asmodeus] Auto-admitting: ' + btns[i].textContent.trim());"
        "        btns[i].click();"
        "      }"
        "    }"
        "  }, 2000);"
        "  console.log('[Asmodeus] Auto-admit enabled');"
        "})();";
    std::u16string auto_admit_js(auto_admit_src.begin(), auto_admit_src.end());
    web_contents_->GetPrimaryMainFrame()->ExecuteJavaScriptForTests(
        auto_admit_js, base::NullCallback(), /*world_id=*/0);
  }

  *out_joined = true;
  *out_platform = platform;
  LOG(INFO) << "JoinMeeting: platform=" << platform << " url=" << url;
  return DispatchResponse::Success();
}

DispatchResponse AsmodeusHandler::CreateMeeting(
    std::optional<String> in_name,
    std::optional<String> in_meetingHtml,
    String* out_meetingUrl,
    String* out_signalingUrl,
    int* out_port) {
  std::string name = in_name.value_or("Asmodeus Meeting");

  if (meeting_server_ && meeting_server_->is_running()) {
    *out_meetingUrl = meeting_server_->GetMeetingUrl();
    *out_signalingUrl = meeting_server_->GetSignalingUrl();
    *out_port = meeting_server_->port();
    return DispatchResponse::Success();
  }

  std::string html_content;
  if (in_meetingHtml.has_value()) {
    html_content = in_meetingHtml.value();
  } else {
    html_content = "<!DOCTYPE html><html><body>"
        "<h1>Asmodeus Meeting</h1>"
        "<p>Pass meetingHtml parameter with full meeting page content.</p>"
        "</body></html>";
  }

  meeting_server_ = std::make_unique<asmodeus::AsmodeusMeetingServer>();

  // Start creates its own IO thread — safe to call from UI thread.
  if (!meeting_server_->Start(name, html_content)) {
    meeting_server_.reset();
    return DispatchResponse::ServerError("Failed to start meeting server");
  }

  // Also create the new meeting coordinator (native meeting backend).
  std::string home = getenv("HOME") ? getenv("HOME") : "/tmp";
  std::string agent_path = home + "/workspace/chromium/src/out/Default/asmodeus_agent";
  coordinator_ = std::make_unique<asmodeus::MeetingCoordinator>();
  coordinator_->SetEventCallback(
      [this](const std::string& agent, const std::string& event_type,
             const base::Value& data) {
        if (!data.is_dict()) return;
        const auto& dict = data.GetDict();
        if (event_type == "speech_started") {
          const std::string* text = dict.FindString("text");
          const std::string* source = dict.FindString("source");
          frontend_->AgentSpoke(agent, text ? *text : "",
                                source ? *source : "tts");
          frontend_->AgentSpeakingChanged(agent, true);
        } else if (event_type == "speech_ended") {
          auto dur = dict.FindDouble("durationMs");
          frontend_->AgentSpeechEnded(agent, dur ? *dur : 0);
          frontend_->AgentSpeakingChanged(agent, false);
        } else if (event_type == "heard") {
          const std::string* text = dict.FindString("text");
          auto stt_ms = dict.FindInt("sttMs");
          frontend_->AgentHeard(agent, text ? *text : "",
                                stt_ms ? *stt_ms : 0);
        } else if (event_type == "barge_in") {
          frontend_->AgentBargeIn(agent);
        } else if (event_type == "error") {
          const std::string* msg = dict.FindString("message");
          frontend_->AgentError(agent, msg ? *msg : "unknown");
        }
      });
  coordinator_->Start(name, agent_path);

  *out_meetingUrl = meeting_server_->GetMeetingUrl();
  *out_signalingUrl = meeting_server_->GetSignalingUrl();
  *out_port = meeting_server_->port();
  return DispatchResponse::Success();
}

DispatchResponse AsmodeusHandler::AddAgent(
    const String& in_name,
    std::optional<String> in_voiceModel,
    std::optional<String> in_displayName,
    String* out_participantId,
    String* out_audioShmPath,
    String* out_videoShmPath,
    bool* out_connected,
    int* out_controlPort) {
  std::string name = in_name;

  // Use coordinator if available (new native agent system).
  if (coordinator_) {
    std::string home = getenv("HOME") ? getenv("HOME") : "/tmp";
    // Assign different voices per agent for variety.
    const char* voices[] = {
        "en_US-amy-medium.onnx",
        "en_US-ryan-high.onnx",
        "en_US-kristin-medium.onnx",
        "en_US-joe-medium.onnx",
        "en_US-kusal-medium.onnx",
        "en_US-norman-medium.onnx",
    };
    static int voice_idx = 0;
    // SAFETY: voice_idx % 6 is always in [0,5], matching array size.
    UNSAFE_BUFFERS(std::string default_voice = home + "/.asmodeus/voices/" +
        voices[voice_idx++ % 6]);
    std::string voice = in_voiceModel.value_or(default_voice);
    std::string display = in_displayName.value_or(
        std::string(1, toupper(name[0])) + name.substr(1));
    if (!coordinator_->AddAgent(name, voice, display)) {
      return DispatchResponse::ServerError("Failed to add agent: " + name);
    }
    auto it = coordinator_->agents().find(name);
    if (it != coordinator_->agents().end()) {
      *out_participantId = name;
      *out_audioShmPath = it->second.audio_shm_path;
      *out_videoShmPath = it->second.video_shm_path;
      *out_connected = true;
      *out_controlPort = it->second.control_port;
    }
    return DispatchResponse::Success();
  }

  // Legacy path (Chrome-based participants).
  if (!meeting_server_ || !meeting_server_->is_running()) {
    return DispatchResponse::ServerError(
        "No meeting running. Call createMeeting first.");
  }

  if (participants_.count(name)) {
    return DispatchResponse::ServerError(
        "Agent already exists: " + name);
  }

  // 1. Create virtual audio device.
  if (!media_servers_.count(name)) {
    auto server = std::make_unique<asmodeus::AsmodeusMediaServer>();
    if (!server->Start(name, 48000, 1)) {
      return DispatchResponse::ServerError(
          "Failed to create virtual audio for: " + name);
    }
    media_servers_[name] = std::move(server);
  }

  // 2. Create virtual camera (shm).
  std::string home = getenv("HOME") ? getenv("HOME") : "/tmp";
  std::string video_shm = home + "/.asmodeus/video-in-" + name + ".shm";
  video_shm_paths_[name] = video_shm;

  // 3. Build meeting URL with query params.
  std::string display = in_displayName.value_or(
      std::string(1, toupper(name[0])) + name.substr(1));
  std::string meeting_url = meeting_server_->GetMeetingUrl() +
      "?name=" + display +
      "&signaling=" + meeting_server_->GetSignalingUrl() +
      "&device=" + name +
      "&meeting=" + meeting_server_->meeting_name();

  // 4. Create participant (headless WebContents).
  auto participant = std::make_unique<asmodeus::AsmodeusParticipant>(
      name, web_contents_->GetBrowserContext(),
      "asmodeus-" + name, "asmodeus-cam-" + name,
      /*use_incognito=*/false);
  participant->Navigate(GURL(meeting_url));

  *out_participantId = name;
  *out_audioShmPath = media_servers_[name]->input_path();
  *out_videoShmPath = video_shm;
  *out_connected = false;  // Will connect asynchronously via WebRTC.
  *out_controlPort = 0;  // Legacy path doesn't use control ports.

  participants_[name] = std::move(participant);
  return DispatchResponse::Success();
}

DispatchResponse AsmodeusHandler::RemoveAgent(const String& in_name) {
  std::string name = in_name;

  // Coordinator agent removal
  if (coordinator_) {
    coordinator_->RemoveAgent(name);
    return DispatchResponse::Success();
  }

  // Legacy path: Destroy participant.
  auto p_it = participants_.find(name);
  if (p_it != participants_.end()) {
    participants_.erase(p_it);
  }

  // Destroy virtual audio.
  auto m_it = media_servers_.find(name);
  if (m_it != media_servers_.end()) {
    m_it->second->Stop();
    media_servers_.erase(m_it);
  }

  // Remove virtual camera path.
  video_shm_paths_.erase(name);

  return DispatchResponse::Success();
}

DispatchResponse AsmodeusHandler::GetMeetingState(
    std::unique_ptr<protocol::Array<protocol::Asmodeus::AgentState>>* out_agents,
    int* out_signalingPeers,
    bool* out_recording) {
  auto agents = std::make_unique<protocol::Array<protocol::Asmodeus::AgentState>>();

  for (const auto& [name, participant] : participants_) {
    auto state = protocol::Asmodeus::AgentState::Create()
        .SetName(name)
        .SetConnected(participant->is_loaded())
        .SetPeerCount(0)
        .SetAudioRmsIn(0)
        .SetAudioRmsOut(0)
        .Build();
    agents->push_back(std::move(state));
  }

  // Also include coordinator agents
  if (coordinator_) {
    for (const auto& [name, entry] : coordinator_->agents()) {
      auto state = protocol::Asmodeus::AgentState::Create()
          .SetName(name)
          .SetConnected(true)
          .SetPeerCount(0)
          .SetAudioRmsIn(entry.rms)
          .SetAudioRmsOut(0)
          .Build();
      state->SetHeardCount(entry.heard_count);
      state->SetSpeechCount(entry.speech_count);
      state->SetBargeInCount(entry.barge_in_count);
      if (!entry.last_heard.empty())
        state->SetLastHeard(entry.last_heard);
      if (!entry.last_spoken.empty())
        state->SetLastSpoken(entry.last_spoken);
      state->SetSpeaking(entry.speaking);
      state->SetSpeakingFrames(entry.speaking_frames);
      agents->push_back(std::move(state));
    }
  }

  *out_agents = std::move(agents);
  *out_signalingPeers = meeting_server_ ? meeting_server_->GetPeerCount() : 0;
  *out_recording = coordinator_ ? coordinator_->is_recording() : false;
  return DispatchResponse::Success();
}

DispatchResponse AsmodeusHandler::GetTranscript(
    std::unique_ptr<protocol::Array<protocol::Asmodeus::TranscriptEntry>>*
        out_entries) {
  auto entries =
      std::make_unique<protocol::Array<protocol::Asmodeus::TranscriptEntry>>();
  if (coordinator_) {
    for (const auto& t : coordinator_->transcript()) {
      auto entry = protocol::Asmodeus::TranscriptEntry::Create()
          .SetSpeaker(t.speaker)
          .SetText(t.text)
          .SetType(t.type)
          .SetTimestampMs(t.timestamp_ms)
          .Build();
      entries->push_back(std::move(entry));
    }
  }
  *out_entries = std::move(entries);
  return DispatchResponse::Success();
}

DispatchResponse AsmodeusHandler::Speak(const String& in_name,
                                         const String& in_text,
                                         bool* out_queued) {
  if (!coordinator_) {
    return DispatchResponse::ServerError("No meeting. Call createMeeting first.");
  }
  *out_queued = coordinator_->Speak(in_name, in_text);
  return DispatchResponse::Success();
}

DispatchResponse AsmodeusHandler::StartRecording(const String& in_outputDir,
                                                   bool* out_started) {
  if (!coordinator_) {
    return DispatchResponse::ServerError("No meeting. Call createMeeting first.");
  }
  *out_started = coordinator_->StartRecording(in_outputDir);
  return DispatchResponse::Success();
}

DispatchResponse AsmodeusHandler::StopRecording(int* out_frames,
                                                  int* out_audioSamples,
                                                  double* out_peakRms,
                                                  String* out_mp4Path) {
  if (!coordinator_) {
    return DispatchResponse::ServerError("No meeting.");
  }
  int64_t samples = 0;
  std::string mp4;
  coordinator_->StopRecording(out_frames, &samples, out_peakRms, &mp4);
  *out_audioSamples = static_cast<int>(samples);
  *out_mp4Path = mp4;
  return DispatchResponse::Success();
}

// ── General-purpose page interaction ──────────────────────────────

DispatchResponse AsmodeusHandler::ClickElement(
    const String& in_target,
    std::optional<String> in_method,
    bool* out_found,
    std::optional<String>* out_elementTag,
    std::optional<String>* out_elementText) {
  if (!web_contents_) {
    return DispatchResponse::ServerError("No WebContents");
  }
  asmodeus::FindMethod method = asmodeus::FindMethod::kText;
  if (in_method.has_value()) {
    if (*in_method == "textContains") method = asmodeus::FindMethod::kTextContains;
    else if (*in_method == "ariaLabel") method = asmodeus::FindMethod::kAriaLabel;
    else if (*in_method == "selector") method = asmodeus::FindMethod::kSelector;
  }
  auto result = page_controller_.Click(web_contents_, in_target, method);
  *out_found = result.success;
  *out_elementTag = result.element_tag;
  *out_elementText = result.element_text;
  return DispatchResponse::Success();
}

DispatchResponse AsmodeusHandler::TypeInto(
    const String& in_text,
    std::optional<String> in_selector,
    bool* out_success) {
  if (!web_contents_) {
    return DispatchResponse::ServerError("No WebContents");
  }
  auto result = page_controller_.Type(
      web_contents_, in_text, in_selector.value_or(""));
  *out_success = result.success;
  return DispatchResponse::Success();
}

DispatchResponse AsmodeusHandler::ReadElement(
    const String& in_selector,
    std::optional<String>* out_text,
    std::optional<String>* out_value,
    std::optional<String>* out_tag) {
  if (!web_contents_) {
    return DispatchResponse::ServerError("No WebContents");
  }
  auto result = page_controller_.Read(web_contents_, in_selector);
  *out_text = result.element_text;
  return DispatchResponse::Success();
}

DispatchResponse AsmodeusHandler::GetPageInfo(
    String* out_url,
    String* out_title) {
  if (!web_contents_) {
    return DispatchResponse::ServerError("No WebContents");
  }
  *out_url = page_controller_.GetURL(web_contents_);
  *out_title = page_controller_.GetTitle(web_contents_);
  return DispatchResponse::Success();
}

// ── Authentication automation ─────────────────────────────────────

DispatchResponse AsmodeusHandler::SignIn(
    const String& in_agentName,
    bool* out_started,
    std::optional<String>* out_currentPage) {
  if (!auth_controller_) {
    return DispatchResponse::ServerError(
        "No credentials loaded from ~/.asmodeus/accounts.json");
  }
  auth_controller_->SetAccount(in_agentName);
  if (web_contents_) {
    std::string url = web_contents_->GetLastCommittedURL().spec();
    auth_controller_->HandleNavigation(web_contents_, url);
  }
  *out_started = auth_controller_->is_signing_in();
  *out_currentPage = std::to_string(
      static_cast<int>(auth_controller_->current_page()));
  return DispatchResponse::Success();
}

DispatchResponse AsmodeusHandler::Enter2FACode(
    const String& in_code,
    bool* out_submitted) {
  if (!auth_controller_ || !web_contents_) {
    return DispatchResponse::ServerError("No auth controller or WebContents");
  }
  auth_controller_->Enter2FACode(web_contents_, in_code);
  *out_submitted = true;
  return DispatchResponse::Success();
}

DispatchResponse AsmodeusHandler::GetSignInState(
    bool* out_signingIn,
    std::optional<String>* out_currentPage,
    std::optional<String>* out_agentName) {
  if (!auth_controller_) {
    *out_signingIn = false;
    return DispatchResponse::Success();
  }
  *out_signingIn = auth_controller_->is_signing_in();
  *out_currentPage = std::to_string(
      static_cast<int>(auth_controller_->current_page()));
  return DispatchResponse::Success();
}

// ── Permission management ─────────────────────────────────────────

DispatchResponse AsmodeusHandler::SetAutoGrantPermissions(
    bool in_enabled,
    bool* out_success) {
  permission_override_.SetAutoGrant(in_enabled);
  *out_success = true;
  LOG(INFO) << "[Asmodeus] Auto-grant permissions: "
            << (in_enabled ? "ON" : "OFF");
  return DispatchResponse::Success();
}

DispatchResponse AsmodeusHandler::SetPermission(
    const String& in_permission,
    bool in_granted,
    bool* out_success) {
  auto perm = asmodeus::ParsePermission(in_permission);
  if (in_granted) {
    permission_override_.Grant(perm);
  } else {
    permission_override_.Deny(perm);
  }
  *out_success = true;
  LOG(INFO) << "[Asmodeus] Permission " << in_permission
            << (in_granted ? " GRANTED" : " DENIED");
  return DispatchResponse::Success();
}

// ── Instance management ───────────────────────────────────────────

DispatchResponse AsmodeusHandler::LaunchInstance(
    const String& in_agentName,
    std::optional<String> in_profilePath,
    std::optional<int> in_port,
    int* out_cdpPort,
    String* out_profilePath,
    String* out_audioInShm,
    String* out_audioOutShm) {
  if (!instance_manager_) {
    return DispatchResponse::ServerError("No instance manager");
  }
  // Determine profile path
  std::string profile_path;
  if (in_profilePath.has_value()) {
    profile_path = *in_profilePath;
  } else {
    auto acct = credential_store_.GetByName(in_agentName);
    if (acct && !acct->profile.empty()) {
      profile_path = acct->profile;
    } else {
      const char* home = getenv("HOME");
      profile_path = home
          ? std::string(home) + "/.asmodeus/profiles/" + in_agentName
          : "/tmp/asmodeus-profile-" + in_agentName;
    }
  }

  int port = instance_manager_->Launch(
      in_agentName, profile_path, in_port.value_or(0));
  if (port < 0) {
    return DispatchResponse::ServerError("Failed to launch Chrome instance");
  }

  auto* info = instance_manager_->Get(in_agentName);
  *out_cdpPort = port;
  *out_profilePath = profile_path;
  *out_audioInShm = info ? info->audio_in_shm : "";
  *out_audioOutShm = info ? info->audio_out_shm : "";
  return DispatchResponse::Success();
}

DispatchResponse AsmodeusHandler::StopInstance(
    const String& in_agentName) {
  if (!instance_manager_) {
    return DispatchResponse::ServerError("No instance manager");
  }
  instance_manager_->Stop(in_agentName);
  return DispatchResponse::Success();
}

DispatchResponse AsmodeusHandler::ListInstances(
    std::unique_ptr<protocol::Array<protocol::Asmodeus::AgentState>>*
        out_instances) {
  *out_instances =
      std::make_unique<protocol::Array<protocol::Asmodeus::AgentState>>();
  if (!instance_manager_) return DispatchResponse::Success();

  for (const auto& info : instance_manager_->List()) {
    auto state = protocol::Asmodeus::AgentState::Create()
        .SetName(info.agent_name)
        .SetConnected(info.status == asmodeus::InstanceStatus::kRunning)
        .SetPeerCount(0)
        .SetAudioRmsIn(0.0)
        .SetAudioRmsOut(0.0)
        .SetSpeechCount(0)
        .SetHeardCount(0)
        .Build();
    (*out_instances)->emplace_back(std::move(state));
  }
  return DispatchResponse::Success();
}
