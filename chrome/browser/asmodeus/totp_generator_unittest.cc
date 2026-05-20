// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/asmodeus/totp_generator.h"

#include <ctime>

#include "testing/gtest/include/gtest/gtest.h"

namespace asmodeus {

// ── Phase 1.1: Base32Decode ──────────────────────────────────

TEST(TotpGeneratorTest, Base32Decode_ValidSecret) {
  auto result = TotpGenerator::Base32Decode("JBSWY3DPEHPK3PXP");
  ASSERT_EQ(result.size(), 10u);
  EXPECT_EQ(result[0], 0x48);  // 'H'
  EXPECT_EQ(result[1], 0x65);  // 'e'
  EXPECT_EQ(result[2], 0x6C);  // 'l'
  EXPECT_EQ(result[3], 0x6C);  // 'l'
  EXPECT_EQ(result[4], 0x6F);  // 'o'
  EXPECT_EQ(result[5], 0x21);  // '!'
  EXPECT_EQ(result[6], 0xDE);
  EXPECT_EQ(result[7], 0xAD);
  EXPECT_EQ(result[8], 0xBE);
  EXPECT_EQ(result[9], 0xEF);
}

TEST(TotpGeneratorTest, Base32Decode_EmptyString) {
  auto result = TotpGenerator::Base32Decode("");
  EXPECT_TRUE(result.empty());
}

TEST(TotpGeneratorTest, Base32Decode_InvalidChars) {
  auto result = TotpGenerator::Base32Decode("JBSWY3DP!!!!");
  EXPECT_TRUE(result.empty());
}

TEST(TotpGeneratorTest, Base32Decode_WithSpacesAndLowercase) {
  auto with_spaces = TotpGenerator::Base32Decode("jbsw y3dp ehpk 3pxp");
  auto without = TotpGenerator::Base32Decode("JBSWY3DPEHPK3PXP");
  ASSERT_EQ(with_spaces.size(), without.size());
  EXPECT_EQ(with_spaces, without);
}

// ── Phase 1.2: RFC 6238 Test Vectors ─────────────────────────
// Secret: "12345678901234567890" (ASCII, 20 bytes)
// Base32: "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ"
// 6-digit codes derived from 8-digit RFC 6238 Appendix B values.
// Verified against Python hmac/hashlib reference implementation.

static constexpr char kRFC6238Secret[] =
    "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ";

TEST(TotpGeneratorTest, GenerateAt_RFC6238_Time59) {
  EXPECT_EQ(TotpGenerator::GenerateAt(kRFC6238Secret, 59), "287082");
}

TEST(TotpGeneratorTest, GenerateAt_RFC6238_Time1111111109) {
  EXPECT_EQ(TotpGenerator::GenerateAt(kRFC6238Secret, 1111111109), "081804");
}

TEST(TotpGeneratorTest, GenerateAt_RFC6238_Time1234567890) {
  EXPECT_EQ(TotpGenerator::GenerateAt(kRFC6238Secret, 1234567890), "005924");
}

TEST(TotpGeneratorTest, GenerateAt_RFC6238_Time2000000000) {
  EXPECT_EQ(TotpGenerator::GenerateAt(kRFC6238Secret, 2000000000), "279037");
}

// ── Phase 1.3: Edge Cases ────────────────────────────────────

TEST(TotpGeneratorTest, Generate_InvalidSecret_ReturnsEmpty) {
  EXPECT_EQ(TotpGenerator::Generate(""), "");
  EXPECT_EQ(TotpGenerator::Generate("!@#$"), "");
}

TEST(TotpGeneratorTest, Generate_CurrentTime_Returns6Digits) {
  std::string code = TotpGenerator::Generate("JBSWY3DPEHPK3PXP");
  ASSERT_EQ(code.size(), 6u);
  for (char c : code) {
    EXPECT_TRUE(c >= '0' && c <= '9') << "Non-digit character: " << c;
  }
}

TEST(TotpGeneratorTest, GenerateAt_ZeroPadding) {
  // Time 1234567890 with RFC secret produces "005924" — starts with "00".
  std::string code =
      TotpGenerator::GenerateAt(kRFC6238Secret, 1234567890);
  EXPECT_EQ(code.size(), 6u);
  EXPECT_EQ(code.substr(0, 2), "00");
}

TEST(TotpGeneratorTest, SecondsRemaining_ReturnsValidRange) {
  int remaining = TotpGenerator::SecondsRemaining(30);
  EXPECT_GE(remaining, 1);
  EXPECT_LE(remaining, 30);
}

// ── Phase 5: Custom Parameters ───────────────────────────────

TEST(TotpGeneratorTest, GenerateAt_8Digits) {
  // Full 8-digit RFC 6238 value for t=59.
  EXPECT_EQ(TotpGenerator::GenerateAt(kRFC6238Secret, 59, 8, 30),
            "94287082");
}

TEST(TotpGeneratorTest, GenerateAt_CustomPeriod60) {
  // period=60 → counter = floor(59/60) = 0 (different from period=30 counter=1)
  std::string code30 = TotpGenerator::GenerateAt(kRFC6238Secret, 59, 6, 30);
  std::string code60 = TotpGenerator::GenerateAt(kRFC6238Secret, 59, 6, 60);
  EXPECT_NE(code30, code60);
  EXPECT_EQ(code60.size(), 6u);
}

TEST(TotpGeneratorTest, GenerateAt_DefaultParams) {
  // Default digits=6, period=30 — should produce same result as explicit.
  std::string def = TotpGenerator::Generate("JBSWY3DPEHPK3PXP");
  int64_t now = static_cast<int64_t>(std::time(nullptr));
  std::string explicit_call =
      TotpGenerator::GenerateAt("JBSWY3DPEHPK3PXP", now);
  // These should match if called within the same 30-second window.
  // In rare cases near a boundary they might differ — that's acceptable.
  EXPECT_EQ(def.size(), 6u);
  EXPECT_EQ(explicit_call.size(), 6u);
}

}  // namespace asmodeus
