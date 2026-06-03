// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/handles/profile/credentials_handle.h"

#include <memory>
#include <optional>

#include "base/json/json_writer.h"
#include "base/values.h"
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
      // Read view — deliberately omits the password.
      auto quote = [](const std::string& s) {
        std::string out;
        base::JSONWriter::Write(base::Value(s), &out);
        return out;
      };
      std::string json = "{\"name\":" + quote(acct->name) +
                         ",\"email\":" + quote(acct->email) +
                         ",\"voiceModel\":" + quote(acct->voice_model) +
                         ",\"profile\":" + quote(acct->profile) + "}";
      return ValueHandle::make(Value(json));
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

std::string CredentialsHandleAsk(void* handle,
                                 const std::string& verb,
                                 const std::string& param) {
  auto* holder = static_cast<Holder*>(handle);
  if (!holder || !holder->handle) {
    return "broken:null-handle";
  }
  Value spec = param.empty() ? Value(std::string()) : Value(param);
  std::shared_ptr<Handle> result = holder->handle->ask(verb, spec);
  if (!result) {
    return "broken:null";
  }
  if (result->state_kind() == StateKind::Broken) {
    return std::string("broken:") + std::string(result->broken_reason());
  }
  const Value& v = result->resolved_value();
  if (v.is_int()) {
    return std::to_string(v.as_int());
  }
  if (v.is_string()) {
    return v.as_string();
  }
  return "{}";
}

}  // namespace aurelian
