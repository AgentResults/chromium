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

#include "aurelian/handles/root/wire_reply.h"

namespace asmodeus {
class CredentialStore;
}

namespace aurelian {

// Creates a Velite handle wrapping `store` (which must outlive the handle).
// Returns an opaque pointer (the shared_ptr<Handle> is hidden).
void* CreateCredentialsHandle(asmodeus::CredentialStore* store);
void DestroyCredentialsHandle(void* handle);

// Dispatches an ask through the Velite handle and renders the reply with the
// ONE wire serializer (handles/root/wire_serialize.h) — this shim owns no
// serialization of its own. Verbs:
//   "size"            -> the account count, as a number
//   "phone"           -> the 2FA phone string
//   "account" + name  -> an OBJECT {name,email,voiceModel,profile}
// A refusal comes back Kind::kBroken carrying its bare reason ("not-found",
// "bad-spec", "not-callable", "null-handle"); it is never a value whose text
// begins "broken:". WireReply is velite-free, so this header stays so too.
WireReply CredentialsHandleAsk(void* handle,
                               const std::string& verb,
                               const std::string& param);

}  // namespace aurelian

#endif  // AURELIAN_HANDLES_PROFILE_CREDENTIALS_HANDLE_H_
