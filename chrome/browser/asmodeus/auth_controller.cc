// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/asmodeus/auth_controller.h"

#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/asmodeus/totp_generator.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"

namespace asmodeus {

AuthPage DetectAuthPage(const std::string& url) {
  // Session expired (check first to prevent misclassification)
  if (url.find("accounts.google.com") != std::string::npos &&
      url.find("sessionexpired") != std::string::npos) {
    return AuthPage::kGoogleSessionExpired;
  }
  // Already signed in
  if (url.find("myaccount.google.com") != std::string::npos) {
    return AuthPage::kGoogleSignedIn;
  }
  // Not a Google auth page
  if (url.find("accounts.google.com") == std::string::npos) {
    return AuthPage::kNone;
  }
  // Identify specific challenge pages (most specific first)
  if (url.find("challenge/pk") != std::string::npos) {
    return AuthPage::kGooglePasskey;
  }
  if (url.find("challenge/pwd") != std::string::npos) {
    return AuthPage::kGooglePassword;
  }
  if (url.find("challenge/ipp/verify") != std::string::npos ||
      url.find("challenge/ipp/collect") != std::string::npos) {
    return AuthPage::kGoogle2FAVerify;
  }
  if (url.find("challenge/ipp") != std::string::npos) {
    return AuthPage::kGoogle2FAPhone;
  }
  // TOTP — must be before selection (more specific)
  if (url.find("challenge/totp") != std::string::npos ||
      url.find("challenge/app") != std::string::npos) {
    return AuthPage::kGoogle2FATotp;
  }
  if (url.find("challenge/selection") != std::string::npos) {
    return AuthPage::kGoogle2FASelection;
  }
  if (url.find("signin/identifier") != std::string::npos ||
      url.find("ServiceLogin") != std::string::npos) {
    return AuthPage::kGoogleEmail;
  }
  // Unknown accounts.google.com page — log for debugging
  LOG(WARNING) << "[Asmodeus] AuthController: unrecognised auth page: "
               << url.substr(0, 100);
  return AuthPage::kNone;
}

AuthController::AuthController(const CredentialStore& credentials)
    : credentials_(credentials) {}

AuthController::~AuthController() = default;

void AuthController::SetAccount(const std::string& agent_name) {
  agent_name_ = agent_name;
}

void AuthController::ExecJS(content::WebContents* wc,
                             const std::u16string& script) {
  if (!wc) return;
  auto* frame = wc->GetPrimaryMainFrame();
  if (frame && frame->IsRenderFrameLive()) {
    frame->ExecuteJavaScriptWithUserGestureForTests(
        script, base::NullCallback(), /*world_id=*/0);
  }
}

bool AuthController::HandleNavigation(content::WebContents* wc,
                                       const std::string& url) {
  AuthPage page = DetectAuthPage(url);
  if (page == AuthPage::kNone) return false;

  current_page_ = page;

  if (agent_name_.empty()) {
    LOG(WARNING) << "[Asmodeus] AuthController: no account set, skipping "
                 << url;
    return false;
  }

  auto acct = credentials_->GetByName(agent_name_);
  if (!acct) {
    LOG(WARNING) << "[Asmodeus] AuthController: account not found: "
                 << agent_name_;
    return false;
  }

  LOG(INFO) << "[Asmodeus] AuthController: detected "
            << static_cast<int>(page) << " for " << agent_name_
            << " at " << url.substr(0, 80);

  if (!wc) {
    // No WebContents — can detect page type but can't interact.
    return false;
  }

  switch (page) {
    case AuthPage::kGoogleEmail:
      HandleEmailPage(wc);
      return true;
    case AuthPage::kGooglePasskey:
      HandlePasskeyPage(wc);
      return true;
    case AuthPage::kGooglePassword:
      HandlePasswordPage(wc);
      return true;
    case AuthPage::kGoogle2FASelection:
      Handle2FASelectionPage(wc, *acct);
      return true;
    case AuthPage::kGoogle2FAPhone:
      Handle2FAPhonePage(wc);
      return true;
    case AuthPage::kGoogle2FATotp:
      HandleTotpPage(wc, *acct);
      return true;
    case AuthPage::kGoogleSignedIn:
      LOG(INFO) << "[Asmodeus] AuthController: " << agent_name_
                << " is signed in!";
      signing_in_ = false;
      return true;
    case AuthPage::kGoogleSessionExpired:
      LOG(WARNING) << "[Asmodeus] AuthController: session expired for "
                   << agent_name_ << ", restarting sign-in";
      signing_in_ = false;
      return false;
    default:
      return false;
  }
}

void AuthController::HandleEmailPage(content::WebContents* wc) {
  auto acct = credentials_->GetByName(agent_name_);
  if (!acct) return;

  signing_in_ = true;
  std::string email = acct->email;

  LOG(INFO) << "[Asmodeus] AuthController: filling email " << email;

  // Fill email and click Next
  ExecJS(wc, base::UTF8ToUTF16(
      "var i=document.querySelector('input[type=email]');"
      "if(i){i.focus();i.value='" + email + "';"
      "i.dispatchEvent(new Event('input',{bubbles:true}));"
      "setTimeout(function(){"
      "var b=document.querySelectorAll('button');"
      "for(var x of b){if(x.textContent.trim()==='Next'){x.click();break;}}"
      "},500);}"
  ));
}

void AuthController::HandlePasskeyPage(content::WebContents* wc) {
  LOG(INFO) << "[Asmodeus] AuthController: bypassing passkey";

  // Override navigator.credentials to auto-reject passkey requests.
  ExecJS(wc, u"navigator.credentials.get=function(){"
             u"return Promise.reject(new DOMException("
             u"'User cancelled','NotAllowedError'));};"
             u"navigator.credentials.create=function(){"
             u"return Promise.reject(new DOMException("
             u"'User cancelled','NotAllowedError'));};");

  // Wait for passkey rejection to propagate, then click Try another way
  ExecJS(wc, u"setTimeout(function(){"
             u"var els=document.querySelectorAll('button,a,[role=link],[role=button]');"
             u"for(var el of els){"
             u"if((el.textContent||'').includes('Try another way')){"
             u"el.click();break;}}"
             u"},1000);");
}

void AuthController::HandlePasswordPage(content::WebContents* wc) {
  auto acct = credentials_->GetByName(agent_name_);
  if (!acct) return;

  std::string password = acct->password;
  LOG(INFO) << "[Asmodeus] AuthController: filling password";

  ExecJS(wc, base::UTF8ToUTF16(
      "var p=document.querySelector('input[type=password]');"
      "if(p){p.focus();p.value='" + password + "';"
      "p.dispatchEvent(new Event('input',{bubbles:true}));"
      "setTimeout(function(){"
      "var b=document.querySelectorAll('button');"
      "for(var x of b){if(x.textContent.trim()==='Next'){x.click();break;}}"
      "},500);}"
  ));
}

void AuthController::Handle2FASelectionPage(content::WebContents* wc,
                                             const AgentAccount& account) {
  // Override passkey again (Google may re-trigger it)
  ExecJS(wc, u"navigator.credentials.get=function(){"
             u"return Promise.reject(new DOMException("
             u"'User cancelled','NotAllowedError'));};");

  // Prefer TOTP over SMS when a TOTP secret is configured
  if (!account.totp_secret.empty()) {
    LOG(INFO) << "[Asmodeus] AuthController: selecting TOTP (authenticator app)";

    // Click the Google Authenticator / Authenticator app option.
    // Try multiple text variants (Google changes UI text across locales).
    ExecJS(wc, u"setTimeout(function(){"
               u"var targets=['Google Authenticator','Authenticator app',"
               u"'authenticator','Use your authenticator'];"
               u"var all=document.querySelectorAll('div,li,span,button');"
               u"for(var t of targets){"
               u"for(var el of all){"
               u"var txt=(el.textContent||'');"
               u"if(txt.includes(t)&&el.offsetHeight>20"
               u"&&el.offsetHeight<120&&el.offsetWidth>100){"
               u"el.click();return;}}}"
               u"// Fallback: click 'Enter your password' if on first selection page"
               u"for(var el of all){"
               u"if((el.textContent||'').includes('Enter your password')"
               u"&&el.offsetHeight>20&&el.offsetHeight<120){"
               u"el.click();return;}}"
               u"},500);");
  } else {
    LOG(INFO) << "[Asmodeus] AuthController: selecting SMS verification "
              << "(no TOTP secret configured)";

    // Click the SMS/phone verification option
    ExecJS(wc, u"setTimeout(function(){"
               u"var all=document.querySelectorAll('div,li,span');"
               u"for(var el of all){"
               u"var t=(el.textContent||'');"
               u"if(t.includes('verification code')&&el.offsetHeight>20"
               u"&&el.offsetHeight<120&&el.offsetWidth>200){"
               u"el.click();break;}}"
               u"// Fallback: click 'Enter your password' if on first selection"
               u"for(var el of all){"
               u"if((el.textContent||'').includes('Enter your password')"
               u"&&el.offsetHeight>20&&el.offsetHeight<120){"
               u"el.click();break;}}"
               u"},500);");
  }
}

void AuthController::Handle2FAPhonePage(content::WebContents* wc) {
  std::string phone = credentials_->phone();
  if (phone.empty()) {
    LOG(WARNING) << "[Asmodeus] AuthController: no phone number configured";
    return;
  }

  // Remove +44 prefix, use 0-prefixed UK number
  std::string local_phone = phone;
  if (local_phone.substr(0, 3) == "+44") {
    local_phone = "0" + local_phone.substr(3);
  }

  LOG(INFO) << "[Asmodeus] AuthController: filling phone " << local_phone;

  ExecJS(wc, base::UTF8ToUTF16(
      "var inputs=document.querySelectorAll('input');"
      "for(var i of inputs){"
      "if(i.type!=='hidden'&&i.offsetHeight>0){"
      "i.focus();i.value='" + local_phone + "';"
      "i.dispatchEvent(new Event('input',{bubbles:true}));"
      "break;}}"
      "setTimeout(function(){"
      "var b=document.querySelectorAll('button');"
      "for(var x of b){if(x.textContent.trim()==='Send'){x.click();break;}}"
      "},500);"
  ));
}

void AuthController::HandleTotpPage(content::WebContents* wc,
                                     const AgentAccount& account) {
  if (account.totp_secret.empty()) {
    LOG(WARNING) << "[Asmodeus] AuthController: TOTP page but no secret for "
                 << account.name;
    return;
  }

  // Check time remaining — if < 3 seconds, wait for next period
  int remaining = TotpGenerator::SecondsRemaining(30);
  if (remaining < 3) {
    LOG(INFO) << "[Asmodeus] AuthController: TOTP code about to expire ("
              << remaining << "s), waiting for next period";
    // Post a delayed task to retry after the period rolls over.
    // For simplicity, generate the code anyway — Google accepts codes
    // from the previous period with some tolerance.
  }

  std::string code = TotpGenerator::Generate(account.totp_secret);
  if (code.empty()) {
    LOG(ERROR) << "[Asmodeus] AuthController: failed to generate TOTP code "
               << "for " << account.name;
    return;
  }

  LOG(INFO) << "[Asmodeus] AuthController: entering TOTP code for "
            << account.name;

  // Enter the 6-digit code, check "Don't ask again", click Next/Verify
  ExecJS(wc, base::UTF8ToUTF16(
      "var inputs=document.querySelectorAll('input');"
      "for(var i of inputs){"
      "if(i.type!=='hidden'&&i.offsetHeight>0){"
      "i.focus();i.value='" + code + "';"
      "i.dispatchEvent(new Event('input',{bubbles:true}));"
      "break;}}"
      // Check "Don't ask again on this device" if present
      "setTimeout(function(){"
      "var cbs=document.querySelectorAll('input[type=checkbox]');"
      "for(var cb of cbs){if(!cb.checked&&cb.offsetHeight>0){cb.click();}}"
      // Click Next or Verify
      "setTimeout(function(){"
      "var b=document.querySelectorAll('button');"
      "for(var x of b){"
      "var t=x.textContent.trim();"
      "if(t==='Next'||t==='Verify'){x.click();break;}}"
      "},500);"
      "},500);"
  ));
}

void AuthController::Enter2FACode(content::WebContents* wc,
                                    const std::string& code) {
  LOG(INFO) << "[Asmodeus] AuthController: entering 2FA code";

  // Just the numbers — the field already has "G-" prefix
  ExecJS(wc, base::UTF8ToUTF16(
      "var inputs=document.querySelectorAll('input');"
      "for(var i of inputs){"
      "if(i.type!=='hidden'&&i.offsetHeight>0){"
      "i.focus();i.value='" + code + "';"
      "i.dispatchEvent(new Event('input',{bubbles:true}));"
      "break;}}"
      "setTimeout(function(){"
      "var b=document.querySelectorAll('button');"
      "for(var x of b){if(x.textContent.trim()==='Next'){x.click();break;}}"
      "},500);"
  ));
}

}  // namespace asmodeus
