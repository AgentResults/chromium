// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C10 — the existing asmodeus CredentialStore, now reachable through a
// Velite handle: same data as the direct API, routed via the handle's ask.
//
// TEST-CHANGE (AU-WIRE-KIND): these assertions previously pinned a bare
// std::string reply from a serializer hand-rolled in credentials_handle.cc —
// a second copy of the wire seam, carrying the same two defects the seam had:
//   * a refusal was the string "broken:not-found", indistinguishable from a
//     successful string answer;
//   * `account` built its JSON by string concatenation and returned it as a
//     STRING Value, so structure crossed as text exactly as it did in
//     SerializeWireReply before the kind was carried.
// The duplicate serializer is gone; this drives the ONE seam.

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

  // size: handle == direct API. An int answer is canonical JSON `1`, which is
  // byte-identical to the old bare form — the point being that it is now
  // distinguishable from the STRING "1".
  WireReply size = CredentialsHandleAsk(handle, "size", "");
  EXPECT_FALSE(size.is_broken()) << size;
  EXPECT_EQ(size.payload, std::to_string(store_.size()));
  EXPECT_EQ(size.payload, "1");

  // phone: handle == direct API, and a string answer carries its quotes.
  WireReply phone = CredentialsHandleAsk(handle, "phone", "");
  EXPECT_FALSE(phone.is_broken()) << phone;
  EXPECT_EQ(phone.payload, "\"" + store_.phone() + "\"");
  EXPECT_EQ(phone.payload, "\"+447539492403\"");

  // account: a STRUCTURED answer, not JSON-in-a-string.
  WireReply account = CredentialsHandleAsk(handle, "account", "ultron");
  ASSERT_FALSE(account.is_broken()) << account;
  auto direct = store_.GetByName("ultron");
  ASSERT_TRUE(direct.has_value());
  // An object payload opens with `{` and names its fields as JSON keys — the
  // hand-concatenated form could satisfy neither once quoting was correct.
  ASSERT_FALSE(account.payload.empty());
  EXPECT_EQ(account.payload.front(), '{') << account;
  EXPECT_NE(account.payload.find("\"email\":"), std::string::npos) << account;
  EXPECT_NE(account.payload.find(direct->email), std::string::npos) << account;
  EXPECT_NE(account.payload.find("en_US-joe-medium"), std::string::npos)
      << account;

  // The password must NOT leak through the read handle.
  EXPECT_EQ(account.payload.find("s3cret"), std::string::npos) << account;

  // An unknown account is a REFUSAL, carried in the kind — not a value whose
  // text happens to begin "broken:".
  WireReply missing = CredentialsHandleAsk(handle, "account", "nobody");
  EXPECT_TRUE(missing.is_broken()) << missing;
  EXPECT_EQ(missing.payload, "not-found");

  // A bad spec and an unknown verb are refusals too.
  EXPECT_TRUE(CredentialsHandleAsk(handle, "account", "").is_broken());
  EXPECT_TRUE(CredentialsHandleAsk(handle, "no-such-verb", "").is_broken());

  // A null handle is a refusal, never an empty success.
  WireReply null_reply = CredentialsHandleAsk(nullptr, "size", "");
  EXPECT_TRUE(null_reply.is_broken());
  EXPECT_EQ(null_reply.payload, "null-handle");

  DestroyCredentialsHandle(handle);
}

}  // namespace
}  // namespace aurelian
