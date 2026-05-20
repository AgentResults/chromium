// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/asmodeus/totp_generator.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <ctime>

#include "base/logging.h"
#include "third_party/boringssl/src/include/openssl/digest.h"
#include "third_party/boringssl/src/include/openssl/hmac.h"

namespace asmodeus {

// ── Base32 Decode (RFC 4648) ──────────────────────────────────

std::vector<uint8_t> TotpGenerator::Base32Decode(const std::string& encoded) {
  // Strip spaces and convert to uppercase.
  std::string clean;
  clean.reserve(encoded.size());
  for (char c : encoded) {
    if (c != ' ' && c != '\t' && c != '\n' && c != '-') {
      clean.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
  }

  if (clean.empty()) {
    return {};
  }

  // Strip padding.
  while (!clean.empty() && clean.back() == '=') {
    clean.pop_back();
  }

  // Decode.
  static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
  std::vector<uint8_t> result;
  result.reserve(clean.size() * 5 / 8);

  int buffer = 0;
  int bits_in_buffer = 0;

  for (char c : clean) {
    const char* pos = std::strchr(kAlphabet, c);
    if (!pos) {
      return {};  // Invalid character.
    }
    int value = static_cast<int>(pos - kAlphabet);
    buffer = (buffer << 5) | value;
    bits_in_buffer += 5;
    if (bits_in_buffer >= 8) {
      bits_in_buffer -= 8;
      result.push_back(static_cast<uint8_t>((buffer >> bits_in_buffer) & 0xFF));
    }
  }

  return result;
}

// ── TOTP Generation (RFC 6238 / RFC 4226) ─────────────────────

std::string TotpGenerator::GenerateAt(const std::string& base32_secret,
                                       int64_t unix_timestamp,
                                       int digits,
                                       int period) {
  if (digits < 1 || digits > 10 || period < 1) {
    return {};
  }

  std::vector<uint8_t> key = Base32Decode(base32_secret);
  if (key.empty()) {
    return {};
  }

  // Counter = floor(timestamp / period), as 8-byte big-endian.
  int64_t counter = unix_timestamp / period;
  uint8_t counter_bytes[8];
  for (int i = 7; i >= 0; --i) {
    counter_bytes[i] = static_cast<uint8_t>(counter & 0xFF);
    counter >>= 8;
  }

  // HMAC-SHA1(key, counter).
  uint8_t hash[20];
  unsigned int hash_len = 0;
  if (!HMAC(EVP_sha1(), key.data(), static_cast<int>(key.size()),
            counter_bytes, 8, hash, &hash_len)) {
    LOG(ERROR) << "[Asmodeus] TOTP: HMAC-SHA1 failed";
    return {};
  }

  // Dynamic truncation (RFC 4226 section 5.3).
  int offset = hash[19] & 0x0F;
  uint32_t truncated =
      (static_cast<uint32_t>(hash[offset] & 0x7F) << 24) |
      (static_cast<uint32_t>(hash[offset + 1]) << 16) |
      (static_cast<uint32_t>(hash[offset + 2]) << 8) |
      static_cast<uint32_t>(hash[offset + 3]);

  // Modulo to get the desired number of digits.
  uint64_t modulo = 1;
  for (int i = 0; i < digits; ++i) {
    modulo *= 10;
  }
  uint32_t code = truncated % static_cast<uint32_t>(modulo);

  // Zero-pad to the requested number of digits.
  std::string result = std::to_string(code);
  while (static_cast<int>(result.size()) < digits) {
    result.insert(result.begin(), '0');
  }

  return result;
}

std::string TotpGenerator::Generate(const std::string& base32_secret,
                                     int digits,
                                     int period) {
  int64_t now = static_cast<int64_t>(std::time(nullptr));
  return GenerateAt(base32_secret, now, digits, period);
}

int TotpGenerator::SecondsRemaining(int period) {
  if (period < 1) {
    return 0;
  }
  int64_t now = static_cast<int64_t>(std::time(nullptr));
  int elapsed = static_cast<int>(now % period);
  return period - elapsed;
}

}  // namespace asmodeus
