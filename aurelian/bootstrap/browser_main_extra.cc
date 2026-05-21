// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/bootstrap/browser_main_extra.h"

#include "base/logging.h"
#include "velite/agentspaces-wire/actorspace.hpp"
#include "velite/agentspaces-wire/handle.hpp"
#include "velite/agentspaces-wire/value_handle.hpp"

namespace aurelian {
namespace {

// A minimal root handle for legion://chrome/.
// Answers __getIdentity and describe; everything else is not-callable.
// C1+ will replace this with the full BrowserHandle.
class ChromeRootHandle : public velite::agentspaces::Handle {
 public:
  static std::shared_ptr<ChromeRootHandle> make() {
    return std::shared_ptr<ChromeRootHandle>(new ChromeRootHandle());
  }

  velite::agentspaces::StateKind state_kind() const override {
    return velite::agentspaces::StateKind::ResolvedValue;
  }

  const velite::agentspaces::Value& resolved_value() const override {
    return value_;
  }

  std::shared_ptr<velite::agentspaces::Handle> resolved_handle()
      const override {
    return nullptr;
  }

  std::string_view broken_reason() const override { return ""; }

  std::string sturdy_identity() const override {
    return "legion://chrome/";
  }

  std::shared_ptr<velite::agentspaces::Handle> ask_impl(
      std::string_view msg,
      const velite::agentspaces::Value& /*spec*/) override {
    using V = velite::agentspaces::Value;
    using VH = velite::agentspaces::ValueHandle;

    if (msg == "__getIdentity") {
      return VH::make(V("legion://chrome/"));
    }
    if (msg == "describe") {
      return VH::make(V::make_object({
          {"name", V("chrome")},
          {"uri", V("legion://chrome/")},
          {"embodiment", V("aurelian")},
      }));
    }
    return VH::make_broken("not-callable");
  }

  void tell(std::string_view /*msg*/,
            const velite::agentspaces::Value& /*data*/) override {}

 private:
  ChromeRootHandle() : value_("legion://chrome/") {}
  velite::agentspaces::Value value_;
};

}  // namespace

// Opaque holder so the header stays clean of Velite types.
struct ActorSpaceHolder {
  std::shared_ptr<velite::agentspaces::ActorSpace> space;
  std::shared_ptr<ChromeRootHandle> root;
};

BrowserMainExtra::BrowserMainExtra() = default;
BrowserMainExtra::~BrowserMainExtra() = default;

void BrowserMainExtra::PostCreateThreads() {
  holder_ = std::make_unique<ActorSpaceHolder>();

  // 1. Create the browser-process ActorSpace.
  holder_->space = velite::agentspaces::ActorSpace::make("chrome-browser");

  // 2. Create the root handle at legion://chrome/.
  holder_->root = ChromeRootHandle::make();

  // 3. Prove it works: ask __getIdentity.
  auto identity_handle = holder_->root->ask("__getIdentity",
                                            velite::agentspaces::Value());
  std::string identity_result;
  if (identity_handle &&
      identity_handle->state_kind() ==
          velite::agentspaces::StateKind::ResolvedValue &&
      identity_handle->resolved_value().is_string()) {
    identity_result = identity_handle->resolved_value().as_string();
  } else {
    identity_result = "<failed>";
  }

  LOG(WARNING) << "[aurelian] legion://chrome/ mounted; __getIdentity="
               << identity_result;
}

}  // namespace aurelian
