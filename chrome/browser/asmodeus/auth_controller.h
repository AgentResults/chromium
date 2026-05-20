// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ASMODEUS_AUTH_CONTROLLER_H_
#define CHROME_BROWSER_ASMODEUS_AUTH_CONTROLLER_H_

#include <string>

#include "base/memory/raw_ref.h"
#include "chrome/browser/asmodeus/credential_store.h"

namespace content {
class WebContents;
}

namespace asmodeus {

// Types of Google sign-in pages detected by URL pattern.
enum class AuthPage {
  kNone,              // Not an auth page
  kGoogleEmail,       // accounts.google.com/.../identifier — email entry
  kGooglePasskey,     // accounts.google.com/.../challenge/pk — passkey challenge
  kGooglePassword,    // accounts.google.com/.../challenge/pwd — password entry
  kGoogle2FASelection,// accounts.google.com/.../challenge/selection — choose 2FA
  kGoogle2FAPhone,    // accounts.google.com/.../challenge/ipp — phone verify
  kGoogle2FAVerify,   // accounts.google.com/.../challenge/ipp/verify — code entry
  kGoogleSignedIn,    // myaccount.google.com — already signed in
  kGoogleSessionExpired, // accounts.google.com/.../sessionexpired
  kGoogle2FATotp,     // accounts.google.com/.../challenge/totp or /app — TOTP entry
};

// Detect which Google sign-in page a URL corresponds to.
// Pure function — no side effects, no browser deps. Testable in unit tests.
AuthPage DetectAuthPage(const std::string& url);

// Controller that handles automated Google sign-in.
// Observes navigation events and fills forms/clicks buttons as needed.
//
// Usage:
//   AuthController auth(credentials);
//   // In DidFinishNavigation:
//   auth.HandleNavigation(web_contents, url);
class AuthController {
 public:
  explicit AuthController(const CredentialStore& credentials);
  ~AuthController();

  // Handle a navigation to the given URL. If it's a sign-in page,
  // automatically fill credentials and proceed.
  // Returns true if the page was handled (form filled, button clicked).
  bool HandleNavigation(content::WebContents* web_contents,
                        const std::string& url);

  // Set which account to sign in as (by name).
  void SetAccount(const std::string& agent_name);

  // Manually enter a 2FA code (called when user provides it).
  void Enter2FACode(content::WebContents* web_contents,
                    const std::string& code);

  // Get the current auth state.
  AuthPage current_page() const { return current_page_; }
  bool is_signing_in() const { return signing_in_; }

 private:
  // Handle each type of auth page.
  void HandleEmailPage(content::WebContents* wc);
  void HandlePasskeyPage(content::WebContents* wc);
  void HandlePasswordPage(content::WebContents* wc);
  void Handle2FASelectionPage(content::WebContents* wc,
                              const AgentAccount& account);
  void Handle2FAPhonePage(content::WebContents* wc);
  void HandleTotpPage(content::WebContents* wc,
                      const AgentAccount& account);

  // Execute JS with user gesture in the main frame.
  void ExecJS(content::WebContents* wc, const std::u16string& script);

  const raw_ref<const CredentialStore> credentials_;
  std::string agent_name_;
  AuthPage current_page_ = AuthPage::kNone;
  bool signing_in_ = false;
};

}  // namespace asmodeus

#endif  // CHROME_BROWSER_ASMODEUS_AUTH_CONTROLLER_H_
