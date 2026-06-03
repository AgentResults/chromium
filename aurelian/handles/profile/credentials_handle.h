// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C10: route the existing asmodeus CredentialStore THROUGH a Velite
// handle (legion://chrome/profile/credentials). The handle WRAPS the real
// store (no reimplementation) so all access converges on one model; the legacy
// direct API is unchanged (non-destructive). The C-API shim keeps Velite types
// out of headers Chrome/test code includes.

#ifndef AURELIAN_HANDLES_PROFILE_CREDENTIALS_HANDLE_H_
#define AURELIAN_HANDLES_PROFILE_CREDENTIALS_HANDLE_H_

#include <string>

namespace asmodeus {
class CredentialStore;
}

namespace aurelian {

// Creates a Velite handle wrapping `store` (which must outlive the handle).
// Returns an opaque pointer (the shared_ptr<Handle> is hidden).
void* CreateCredentialsHandle(asmodeus::CredentialStore* store);
void DestroyCredentialsHandle(void* handle);

// Dispatches an ask through the Velite handle and serializes the reply:
//   "size"            -> account count (decimal)
//   "phone"           -> the 2FA phone string
//   "account" + name  -> JSON {name,email,voiceModel,profile} or broken:not-found
// A broken reply is "broken:<reason>".
std::string CredentialsHandleAsk(void* handle,
                                 const std::string& verb,
                                 const std::string& param);

}  // namespace aurelian

#endif  // AURELIAN_HANDLES_PROFILE_CREDENTIALS_HANDLE_H_
