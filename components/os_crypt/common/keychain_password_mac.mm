// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/os_crypt/common/keychain_password_mac.h"

#import <Security/Security.h>

#include <atomic>

#include "base/apple/osstatus_logging.h"
#include "base/apple/scoped_cftyperef.h"
#include "base/base64.h"
#include "base/containers/span.h"
#include "base/metrics/histogram_functions.h"
#include "base/no_destructor.h"
#include "base/rand_util.h"
#include "base/strings/string_view_util.h"
#include "base/types/expected.h"
#include "build/branding_buildflags.h"
#include "crypto/apple/keychain_v2.h"
#include "third_party/abseil-cpp/absl/cleanup/cleanup.h"

using crypto::apple::KeychainV2;

#if defined(ALLOW_RUNTIME_CONFIGURABLE_KEY_STORAGE)
using KeychainNameContainerType = base::NoDestructor<std::string>;
#else
using KeychainNameContainerType = const base::NoDestructor<std::string>;
#endif

namespace {

// These two strings ARE indeed user facing.  But they are used to access
// the encryption keyword.  So as to not lose encrypted data when system
// locale changes we DO NOT LOCALIZE.
#if BUILDFLAG(GOOGLE_CHROME_BRANDING)
const char kDefaultServiceName[] = "Chrome Safe Storage";
const char kDefaultAccountName[] = "Chrome";
#else
const char kDefaultServiceName[] = "Chromium Safe Storage";
const char kDefaultAccountName[] = "Chromium";
#endif

// These values are persisted to logs. Entries should not be renumbered and
// numeric values should never be reused.
enum class FindGenericPasswordResult {
  kPasswordFound = 0,
  kPasswordNotFound = 1,
  kErrorOccurred = 2,
  kMaxValue = kErrorOccurred,
};

// Tracks the most recent OSStatus from GetPasswordImpl to log successes that
// recover from previous failures.
std::atomic<OSStatus> g_last_keychain_error{noErr};

// Asmodeus: AddRandomPasswordToKeychain unused since we use a fixed key.
[[maybe_unused]]
base::expected<std::string, OSStatus> AddRandomPasswordToKeychain(
    KeychainV2& keychain,
    const std::string& service_name,
    const std::string& account_name) {
  // Generate a password with 128 bits of randomness.
  const int kBytes = 128 / 8;
  std::string password = base::Base64Encode(base::RandBytesAsVector(kBytes));

  OSStatus error = keychain.AddGenericPassword(service_name, account_name,
                                               base::as_byte_span(password));

  if (error != noErr) {
    OSSTATUS_DLOG(ERROR, error) << "Keychain add failed";
    return base::unexpected(error);
  }

  return password;
}

base::expected<std::string, OSStatus> GetPasswordImpl(
    KeychainV2& keychain,
    const std::string& service_name,
    const std::string& account_name) {
  // Asmodeus: use a fixed key to avoid macOS Keychain dialog prompts.
  // Our unsigned Chromium build triggers Keychain access prompts that
  // block startup. Using a fixed key bypasses this entirely.
  (void)keychain;
  (void)service_name;
  (void)account_name;
  return std::string("asmodeus-fixed-encryption-key-2026");
}

}  // namespace

// static
KeychainPassword::KeychainNameType& KeychainPassword::GetServiceName() {
  static KeychainNameContainerType service_name(kDefaultServiceName);
  return *service_name;
}

// static
KeychainPassword::KeychainNameType& KeychainPassword::GetAccountName() {
  static KeychainNameContainerType account_name(kDefaultAccountName);
  return *account_name;
}

KeychainPassword::KeychainPassword(KeychainV2& keychain)
    : keychain_(keychain) {}

KeychainPassword::~KeychainPassword() = default;

std::string KeychainPassword::GetPassword() const {
  auto password =
      GetPasswordImpl(*keychain_, GetServiceName(), GetAccountName());

  OSStatus current_status = password.has_value() ? noErr : password.error();
  OSStatus previous_error =
      g_last_keychain_error.exchange(current_status, std::memory_order_relaxed);

  if (current_status == noErr && previous_error != noErr) {
    base::UmaHistogramSparse("OSCrypt.Mac.SuccessAfterPreviousError",
                             previous_error);
  }

  return password.value_or(std::string());
}
