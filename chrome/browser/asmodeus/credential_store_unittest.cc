// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/asmodeus/credential_store.h"

#include <fstream>

#include "base/files/file_path.h"
#include "base/files/scoped_temp_dir.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace asmodeus {

class CredentialStoreTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
    config_path_ = temp_dir_.GetPath().Append("accounts.json").value();
  }

  void WriteConfig(const std::string& content) {
    std::ofstream f(config_path_);
    f << content;
  }

  base::ScopedTempDir temp_dir_;
  std::string config_path_;
};

TEST_F(CredentialStoreTest, LoadsValidConfig) {
  WriteConfig(R"({
    "phone": "+447539492403",
    "accounts": [
      {"name": "ultron", "email": "ultron@ebm.ai", "password": "pass1", "voiceModel": "en_US-joe-medium"},
      {"name": "thanos", "email": "thanos@ebm.ai", "password": "pass2", "voiceModel": "en_US-amy-medium"}
    ]
  })");

  CredentialStore store;
  ASSERT_TRUE(store.Load(config_path_));
  EXPECT_EQ(store.size(), 2u);
  EXPECT_EQ(store.phone(), "+447539492403");
}

TEST_F(CredentialStoreTest, LooksUpByName) {
  WriteConfig(R"({
    "accounts": [
      {"name": "ultron", "email": "ultron@ebm.ai", "password": "pass1"}
    ]
  })");

  CredentialStore store;
  ASSERT_TRUE(store.Load(config_path_));

  auto acct = store.GetByName("ultron");
  ASSERT_TRUE(acct.has_value());
  EXPECT_EQ(acct->name, "ultron");
  EXPECT_EQ(acct->email, "ultron@ebm.ai");
  EXPECT_EQ(acct->password, "pass1");

  EXPECT_FALSE(store.GetByName("nonexistent").has_value());
}

TEST_F(CredentialStoreTest, LooksUpByEmail) {
  WriteConfig(R"({
    "accounts": [
      {"name": "ultron", "email": "ultron@ebm.ai", "password": "p"}
    ]
  })");

  CredentialStore store;
  ASSERT_TRUE(store.Load(config_path_));

  auto acct = store.GetByEmail("ultron@ebm.ai");
  ASSERT_TRUE(acct.has_value());
  EXPECT_EQ(acct->name, "ultron");

  EXPECT_FALSE(store.GetByEmail("nobody@ebm.ai").has_value());
}

TEST_F(CredentialStoreTest, HandlesEmptyAccountsList) {
  WriteConfig(R"({"accounts": []})");

  CredentialStore store;
  ASSERT_TRUE(store.Load(config_path_));
  EXPECT_EQ(store.size(), 0u);
}

TEST_F(CredentialStoreTest, HandlesMissingFile) {
  CredentialStore store;
  EXPECT_FALSE(store.Load("/nonexistent/path/accounts.json"));
  EXPECT_EQ(store.size(), 0u);
}

TEST_F(CredentialStoreTest, HandlesMalformedJson) {
  WriteConfig("this is not json {{{");

  CredentialStore store;
  EXPECT_FALSE(store.Load(config_path_));
  EXPECT_EQ(store.size(), 0u);
}

TEST_F(CredentialStoreTest, SkipsAccountsWithMissingFields) {
  WriteConfig(R"({
    "accounts": [
      {"name": "good", "email": "good@test.com", "password": "p"},
      {"email": "noname@test.com", "password": "p"},
      {"name": "noemail", "password": "p"},
      {"name": "valid", "email": "valid@test.com"}
    ]
  })");

  CredentialStore store;
  ASSERT_TRUE(store.Load(config_path_));
  // Only "good" and "valid" have both name and email
  EXPECT_EQ(store.size(), 2u);
  EXPECT_TRUE(store.GetByName("good").has_value());
  EXPECT_TRUE(store.GetByName("valid").has_value());
}

TEST_F(CredentialStoreTest, ExpandsTildeInProfile) {
  WriteConfig(R"({
    "accounts": [
      {"name": "test", "email": "t@t.com", "profile": "~/.asmodeus/profiles/test"}
    ]
  })");

  CredentialStore store;
  ASSERT_TRUE(store.Load(config_path_));

  auto acct = store.GetByName("test");
  ASSERT_TRUE(acct.has_value());
  // Should NOT start with ~ after expansion
  EXPECT_NE(acct->profile[0], '~');
  EXPECT_TRUE(acct->profile.find("/.asmodeus/profiles/test") !=
              std::string::npos);
}

// ── Phase 2: TOTP Secret Loading ─────────────────────────────

TEST_F(CredentialStoreTest, LoadAccountWithTotpSecret) {
  WriteConfig(R"({
    "accounts": [
      {"name": "test", "email": "t@e.ai", "password": "p",
       "totpSecret": "JBSWY3DPEHPK3PXP"}
    ]
  })");

  CredentialStore store;
  ASSERT_TRUE(store.Load(config_path_));
  auto acct = store.GetByName("test");
  ASSERT_TRUE(acct.has_value());
  EXPECT_EQ(acct->totp_secret, "JBSWY3DPEHPK3PXP");
}

TEST_F(CredentialStoreTest, LoadAccountWithoutTotpSecret) {
  WriteConfig(R"({
    "accounts": [
      {"name": "test", "email": "t@e.ai", "password": "p"}
    ]
  })");

  CredentialStore store;
  ASSERT_TRUE(store.Load(config_path_));
  auto acct = store.GetByName("test");
  ASSERT_TRUE(acct.has_value());
  EXPECT_EQ(acct->totp_secret, "");
}

TEST_F(CredentialStoreTest, LoadAccountWithEmptyTotpSecret) {
  WriteConfig(R"({
    "accounts": [
      {"name": "test", "email": "t@e.ai", "password": "p",
       "totpSecret": ""}
    ]
  })");

  CredentialStore store;
  ASSERT_TRUE(store.Load(config_path_));
  auto acct = store.GetByName("test");
  ASSERT_TRUE(acct.has_value());
  EXPECT_EQ(acct->totp_secret, "");
}

}  // namespace asmodeus
