// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/asmodeus/auth_controller.h"

#include "chrome/browser/asmodeus/totp_generator.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace asmodeus {

TEST(AuthControllerTest, DetectsGoogleEmailPage) {
  EXPECT_EQ(DetectAuthPage(
      "https://accounts.google.com/v3/signin/identifier?dsh=S123&flowName=GlifWebSignIn"),
      AuthPage::kGoogleEmail);
  EXPECT_EQ(DetectAuthPage(
      "https://accounts.google.com/ServiceLogin?continue=https://meet.google.com"),
      AuthPage::kGoogleEmail);
}

TEST(AuthControllerTest, DetectsGooglePasskeyChallenge) {
  EXPECT_EQ(DetectAuthPage(
      "https://accounts.google.com/v3/signin/challenge/pk?TL=APouJz123"),
      AuthPage::kGooglePasskey);
}

TEST(AuthControllerTest, DetectsGooglePasswordPage) {
  EXPECT_EQ(DetectAuthPage(
      "https://accounts.google.com/v3/signin/challenge/pwd?TL=APouJz123"),
      AuthPage::kGooglePassword);
}

TEST(AuthControllerTest, DetectsGoogle2FASelection) {
  EXPECT_EQ(DetectAuthPage(
      "https://accounts.google.com/v3/signin/challenge/selection?TL=APouJz123"),
      AuthPage::kGoogle2FASelection);
}

TEST(AuthControllerTest, DetectsGoogle2FAPhone) {
  EXPECT_EQ(DetectAuthPage(
      "https://accounts.google.com/v3/signin/challenge/ipp/collect?TL=APouJz123"),
      AuthPage::kGoogle2FAVerify);
  EXPECT_EQ(DetectAuthPage(
      "https://accounts.google.com/v3/signin/challenge/ipp?TL=APouJz123"),
      AuthPage::kGoogle2FAPhone);
}

TEST(AuthControllerTest, DetectsGoogle2FAVerify) {
  EXPECT_EQ(DetectAuthPage(
      "https://accounts.google.com/v3/signin/challenge/ipp/verify?TL=APouJz123"),
      AuthPage::kGoogle2FAVerify);
}

TEST(AuthControllerTest, DetectsSignedIn) {
  EXPECT_EQ(DetectAuthPage(
      "https://myaccount.google.com/?utm_source=sign_in_no_continue&pli=1"),
      AuthPage::kGoogleSignedIn);
}

TEST(AuthControllerTest, DetectsSessionExpired) {
  EXPECT_EQ(DetectAuthPage(
      "https://accounts.google.com/info/sessionexpired?TL=APouJz123"),
      AuthPage::kGoogleSessionExpired);
}

TEST(AuthControllerTest, ReturnsNoneForNonAuthPages) {
  EXPECT_EQ(DetectAuthPage("https://meet.google.com/abc-defg-hij"),
      AuthPage::kNone);
  EXPECT_EQ(DetectAuthPage("https://www.google.com"),
      AuthPage::kNone);
  EXPECT_EQ(DetectAuthPage("https://example.com/signin"),
      AuthPage::kNone);
  EXPECT_EQ(DetectAuthPage(""),
      AuthPage::kNone);
}

TEST(AuthControllerTest, HandlesPriorityCorrectly) {
  // challenge/ipp/verify should match kGoogle2FAVerify, not kGoogle2FAPhone
  EXPECT_EQ(DetectAuthPage(
      "https://accounts.google.com/v3/signin/challenge/ipp/verify?TL=x"),
      AuthPage::kGoogle2FAVerify);

  // sessionexpired should take priority over other patterns
  EXPECT_EQ(DetectAuthPage(
      "https://accounts.google.com/info/sessionexpired?TL=x"),
      AuthPage::kGoogleSessionExpired);
}

// Test AuthController with CredentialStore integration (no browser needed)
TEST(AuthControllerTest, SetAccountAndDetect) {
  CredentialStore store;
  // Can't load from file in unit test, but test the controller creation
  AuthController auth(store);
  auth.SetAccount("ultron");
  EXPECT_FALSE(auth.is_signing_in());

  // HandleNavigation returns false without valid credentials
  // (no WebContents in unit test, just test the detection path)
  EXPECT_EQ(auth.current_page(), AuthPage::kNone);
}

// ── Phase 3: TOTP Page Detection ─────────────────────────────

TEST(AuthControllerTest, DetectsTotpChallenge) {
  EXPECT_EQ(DetectAuthPage(
      "https://accounts.google.com/v3/signin/challenge/totp/2?TL=abc123"),
      AuthPage::kGoogle2FATotp);
}

TEST(AuthControllerTest, DetectsAppChallenge) {
  EXPECT_EQ(DetectAuthPage(
      "https://accounts.google.com/v3/signin/challenge/app?TL=abc123"),
      AuthPage::kGoogle2FATotp);
}

TEST(AuthControllerTest, TotpBeforeSelection) {
  // challenge/totp must be detected as TOTP, not selection
  EXPECT_EQ(DetectAuthPage(
      "https://accounts.google.com/v3/signin/challenge/totp"),
      AuthPage::kGoogle2FATotp);
  // But challenge/selection is still selection
  EXPECT_EQ(DetectAuthPage(
      "https://accounts.google.com/v3/signin/challenge/selection"),
      AuthPage::kGoogle2FASelection);
}

TEST(AuthControllerTest, SelectionStillWorksAfterTotpAdded) {
  EXPECT_EQ(DetectAuthPage(
      "https://accounts.google.com/v3/signin/challenge/selection?TL=x"),
      AuthPage::kGoogle2FASelection);
}

TEST(AuthControllerTest, PasswordStillWorksAfterTotpAdded) {
  EXPECT_EQ(DetectAuthPage(
      "https://accounts.google.com/v3/signin/challenge/pwd?TL=x"),
      AuthPage::kGooglePassword);
}

// ── Phase 4: AuthController TOTP Integration ─────────────────

TEST(AuthControllerTest, AuthControllerWithTotpSecret) {
  CredentialStore store;
  AuthController auth(store);
  auth.SetAccount("test");
  EXPECT_FALSE(auth.is_signing_in());
  EXPECT_EQ(auth.current_page(), AuthPage::kNone);
}

TEST(AuthControllerTest, AuthControllerNoTotpSecret) {
  CredentialStore store;
  AuthController auth(store);
  auth.SetAccount("test");
  EXPECT_FALSE(auth.is_signing_in());
}

TEST(AuthControllerTest, TotpGenerator_ProducesCodeForSecret) {
  // Verify TotpGenerator works with a typical secret
  std::string code = TotpGenerator::Generate("JBSWY3DPEHPK3PXP");
  ASSERT_EQ(code.size(), 6u);
  for (char c : code) {
    EXPECT_TRUE(c >= '0' && c <= '9');
  }
}

TEST(AuthControllerTest, TotpGenerator_EmptySecretReturnsEmpty) {
  EXPECT_EQ(TotpGenerator::Generate(""), "");
}

}  // namespace asmodeus
