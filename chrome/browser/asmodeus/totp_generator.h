// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ASMODEUS_TOTP_GENERATOR_H_
#define CHROME_BROWSER_ASMODEUS_TOTP_GENERATOR_H_

#include <cstdint>
#include <string>
#include <vector>

namespace asmodeus {

// RFC 6238 TOTP (Time-based One-Time Password) generator.
// Uses HMAC-SHA1 via BoringSSL. Stateless — all methods are static.
//
// Google Authenticator uses standard TOTP: SHA-1, 6 digits, 30-second period.
class TotpGenerator {
 public:
  // Generate a TOTP code from a base32-encoded secret using current time.
  // Returns zero-padded digit string, or empty string on error.
  static std::string Generate(const std::string& base32_secret,
                               int digits = 6,
                               int period = 30);

  // Generate with explicit Unix timestamp (for deterministic testing).
  static std::string GenerateAt(const std::string& base32_secret,
                                 int64_t unix_timestamp,
                                 int digits = 6,
                                 int period = 30);

  // Seconds remaining until the current code expires. Range: [1, period].
  static int SecondsRemaining(int period = 30);

  // Decode a base32 (RFC 4648) string to raw bytes.
  // Strips spaces, converts to uppercase before decoding.
  // Returns empty vector on invalid input.
  static std::vector<uint8_t> Base32Decode(const std::string& encoded);

  TotpGenerator() = delete;
};

}  // namespace asmodeus

#endif  // CHROME_BROWSER_ASMODEUS_TOTP_GENERATOR_H_
