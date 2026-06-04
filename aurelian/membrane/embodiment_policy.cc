// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/membrane/embodiment_policy.h"

namespace aurelian {

EmbodimentPolicy::EmbodimentPolicy() = default;
EmbodimentPolicy::~EmbodimentPolicy() = default;
EmbodimentPolicy::EmbodimentPolicy(const EmbodimentPolicy&) = default;
EmbodimentPolicy& EmbodimentPolicy::operator=(const EmbodimentPolicy&) =
    default;

// static
EmbodimentPolicy EmbodimentPolicy::WithCapabilities(
    std::set<std::string> allowed,
    std::map<std::string, std::string> scopes) {
  EmbodimentPolicy p;
  p.allowed_ = std::move(allowed);
  p.scopes_ = std::move(scopes);
  return p;
}

// static
EmbodimentPolicy EmbodimentPolicy::FullStandalone() {
  // The capabilities the legion://chrome/ root mounts as children. Kept in
  // sync with ChromeRootHandle's mountable set (root_handle.cc).
  return WithCapabilities({"system", "tabs", "gpu", "media"});
}

bool EmbodimentPolicy::Allows(const std::string& name) const {
  return allowed_.find(name) != allowed_.end();
}

std::string EmbodimentPolicy::ScopeOf(const std::string& name) const {
  auto it = scopes_.find(name);
  return it == scopes_.end() ? std::string() : it->second;
}

}  // namespace aurelian
