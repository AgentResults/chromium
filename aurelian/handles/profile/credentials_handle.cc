// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/handles/profile/credentials_handle.h"

#include <memory>
#include <optional>

#include "aurelian/handles/root/wire_serialize.h"
#include "chrome/browser/asmodeus/credential_store.h"
#include "velite/agentspaces-wire/handle.hpp"
#include "velite/agentspaces-wire/value_handle.hpp"

namespace aurelian {

namespace {

using velite::agentspaces::Handle;
using velite::agentspaces::StateKind;
using velite::agentspaces::Value;
using velite::agentspaces::ValueHandle;

// A Velite handle that delegates every verb to the wrapped CredentialStore.
class CredentialsHandle : public Handle {
 public:
  explicit CredentialsHandle(asmodeus::CredentialStore* store)
      : store_(store) {}

  StateKind state_kind() const override { return StateKind::ResolvedValue; }
  const Value& resolved_value() const override { return identity_; }
  std::shared_ptr<Handle> resolved_handle() const override { return nullptr; }
  std::string_view broken_reason() const override { return ""; }
  std::string sturdy_identity() const override {
    return "legion://chrome/profile/credentials";
  }

  std::shared_ptr<Handle> ask_impl(std::string_view msg,
                                   const Value& spec) override {
    if (msg == "size") {
      return ValueHandle::make(
          Value(static_cast<int64_t>(store_->size())));
    }
    if (msg == "phone") {
      return ValueHandle::make(Value(store_->phone()));
    }
    if (msg == "account") {
      if (!spec.is_string() || spec.as_string().empty()) {
        return ValueHandle::make_broken("bad-spec");
      }
      std::optional<asmodeus::AgentAccount> acct =
          store_->GetByName(spec.as_string());
      if (!acct) {
        return ValueHandle::make_broken("not-found");
      }
      // Read view — deliberately omits the password. A STRUCTURED value:
      // the previous hand-concatenated JSON string made the answer
      // indistinguishable from a string that merely looks like JSON, and
      // duplicated escaping the one marshaller already does.
      return ValueHandle::make(Value::make_object({
          {"name", Value(acct->name)},
          {"email", Value(acct->email)},
          {"voiceModel", Value(acct->voice_model)},
          {"profile", Value(acct->profile)},
      }));
    }
    return ValueHandle::make_broken("not-callable");
  }

  void tell(std::string_view /*msg*/, const Value& /*data*/) override {}

 private:
  asmodeus::CredentialStore* store_;
  Value identity_{std::string("legion://chrome/profile/credentials")};
};

struct Holder {
  std::shared_ptr<Handle> handle;
};

}  // namespace

void* CreateCredentialsHandle(asmodeus::CredentialStore* store) {
  auto* holder = new Holder();
  holder->handle = std::make_shared<CredentialsHandle>(store);
  return holder;
}

void DestroyCredentialsHandle(void* handle) {
  delete static_cast<Holder*>(handle);
}

WireReply CredentialsHandleAsk(void* handle,
                               const std::string& verb,
                               const std::string& param) {
  auto* holder = static_cast<Holder*>(handle);
  if (!holder || !holder->handle) {
    return WireReply::MakeBroken("null-handle");
  }
  Value spec = param.empty() ? Value(std::string()) : Value(param);
  // The ONE serializer. This shim used to carry its own copy, which rendered
  // a refusal as the string "broken:<reason>" and every richer value as text.
  return SerializeWireReply(holder->handle->ask(verb, spec));
}

}  // namespace aurelian
