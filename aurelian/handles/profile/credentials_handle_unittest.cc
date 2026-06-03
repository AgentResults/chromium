// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C10 — the existing asmodeus CredentialStore, now reachable through a
// Velite handle: same data as the direct API, routed via the handle's ask.

#include "aurelian/handles/profile/credentials_handle.h"

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "chrome/browser/asmodeus/credential_store.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace aurelian {
namespace {

constexpr char kAccountsJson[] = R"({
  "accounts": [
    {"name": "ultron", "email": "ultron.agent@ebm.ai",
     "password": "s3cret", "voiceModel": "en_US-joe-medium"}
  ],
  "phone": "+447539492403"
})";

class CredentialsHandleTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(dir_.CreateUniqueTempDir());
    path_ = dir_.GetPath().AppendASCII("accounts.json");
    ASSERT_TRUE(base::WriteFile(path_, kAccountsJson));
    ASSERT_TRUE(store_.Load(path_.AsUTF8Unsafe()));
  }

  base::ScopedTempDir dir_;
  base::FilePath path_;
  asmodeus::CredentialStore store_;
};

TEST_F(CredentialsHandleTest, ReadsThroughHandleMatchDirectApi) {
  void* handle = CreateCredentialsHandle(&store_);
  ASSERT_NE(handle, nullptr);

  // size: handle == direct API.
  EXPECT_EQ(CredentialsHandleAsk(handle, "size", ""),
            std::to_string(store_.size()));
  EXPECT_EQ(CredentialsHandleAsk(handle, "size", ""), "1");

  // phone: handle == direct API.
  EXPECT_EQ(CredentialsHandleAsk(handle, "phone", ""), store_.phone());
  EXPECT_EQ(CredentialsHandleAsk(handle, "phone", ""), "+447539492403");

  // account lookup: routed through the handle, carries the real email.
  std::string account = CredentialsHandleAsk(handle, "account", "ultron");
  auto direct = store_.GetByName("ultron");
  ASSERT_TRUE(direct.has_value());
  EXPECT_NE(account.find(direct->email), std::string::npos) << account;
  EXPECT_NE(account.find("en_US-joe-medium"), std::string::npos) << account;
  // The password must NOT leak through the read handle.
  EXPECT_EQ(account.find("s3cret"), std::string::npos) << account;

  // Unknown account is a broken reply, not a crash.
  EXPECT_EQ(CredentialsHandleAsk(handle, "account", "nobody"),
            "broken:not-found");

  DestroyCredentialsHandle(handle);
}

}  // namespace
}  // namespace aurelian
