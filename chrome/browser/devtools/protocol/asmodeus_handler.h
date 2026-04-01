// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_DEVTOOLS_PROTOCOL_ASMODEUS_HANDLER_H_
#define CHROME_BROWSER_DEVTOOLS_PROTOCOL_ASMODEUS_HANDLER_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "chrome/browser/devtools/protocol/asmodeus.h"
#include "components/password_manager/core/browser/password_store/password_store_consumer.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"

namespace password_manager {
class PasswordStoreInterface;
}

class AsmodeusHandler : public protocol::Asmodeus::Backend,
                        public content::WebContentsObserver {
 public:
  using DispatchResponse = protocol::DispatchResponse;
  using String = protocol::String;

  AsmodeusHandler(protocol::UberDispatcher* dispatcher,
                  content::WebContents* web_contents);
  ~AsmodeusHandler() override;

  AsmodeusHandler(const AsmodeusHandler&) = delete;
  AsmodeusHandler& operator=(const AsmodeusHandler&) = delete;

  DispatchResponse Enable() override;
  DispatchResponse Disable() override;
  DispatchResponse SuppressFingerprints(bool in_suppress) override;

  void GetCredentials(const String& in_origin,
                      std::unique_ptr<GetCredentialsCallback> callback) override;
  void SaveCredentials(const String& in_origin, const String& in_username,
                       const String& in_password,
                       std::optional<String> in_totpSecret,
                       std::optional<String> in_notes,
                       std::unique_ptr<SaveCredentialsCallback> callback) override;
  void DeleteCredentials(const String& in_origin, const String& in_username,
                         std::unique_ptr<DeleteCredentialsCallback> callback) override;
  void ListCredentials(std::unique_ptr<ListCredentialsCallback> callback) override;

  DispatchResponse GenerateTOTP(const String& in_secret,
                                std::optional<int> in_digits,
                                std::optional<int> in_period,
                                String* out_code,
                                double* out_remainingSeconds) override;

  DispatchResponse DetectAuthFlow(
      std::unique_ptr<protocol::Asmodeus::AuthFlow>* out_flow) override;
  DispatchResponse GetAuthState(
      std::unique_ptr<protocol::Array<protocol::Asmodeus::AuthFlow>>*
          out_activeFlows) override;
  DispatchResponse DetectLoginForm(
      std::unique_ptr<protocol::Asmodeus::LoginForm>* out_form) override;

  void AutoLogin(const String& in_origin, std::optional<String> in_username,
                 std::unique_ptr<AutoLoginCallback> callback) override;
  void ExportSession(std::optional<String> in_origin,
                     std::unique_ptr<ExportSessionCallback> callback) override;
  void ImportSession(
      std::unique_ptr<protocol::Array<protocol::Asmodeus::SessionSnapshot>> in_sessions,
      std::unique_ptr<ImportSessionCallback> callback) override;

  void DidStartNavigation(content::NavigationHandle* navigation_handle) override;
  void DidFinishNavigation(content::NavigationHandle* navigation_handle) override;

 private:
  class CredentialConsumer;

  password_manager::PasswordStoreInterface* GetPasswordStore();
  std::string ComputeTOTP(const std::string& base32_secret, int digits,
                          int period, int64_t* remaining_seconds);
  std::vector<uint8_t> Base32Decode(const std::string& input);

  struct AuthProviderMatch { std::string provider; std::string type; };
  std::optional<AuthProviderMatch> MatchAuthProvider(const GURL& url);
  void InjectStealthScript();

  bool enabled_ = false;
  bool fingerprints_suppressed_ = false;

  struct ActiveFlow {
    ActiveFlow();
    ActiveFlow(const ActiveFlow&);
    ActiveFlow(ActiveFlow&&);
    ActiveFlow& operator=(const ActiveFlow&);
    ActiveFlow& operator=(ActiveFlow&&);
    ~ActiveFlow();
    std::string type;
    std::string origin;
    std::string state;
    std::string provider;
  };
  std::vector<ActiveFlow> active_flows_;
  std::string pre_auth_origin_;

  std::vector<std::unique_ptr<CredentialConsumer>> pending_consumers_;
  std::unique_ptr<protocol::Asmodeus::Frontend> frontend_;
  raw_ptr<content::WebContents> web_contents_;
};

#endif  // CHROME_BROWSER_DEVTOOLS_PROTOCOL_ASMODEUS_HANDLER_H_
