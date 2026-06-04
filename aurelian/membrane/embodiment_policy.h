// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian install-membrane (AURELIAN-DESIGN.md §4.5): the C++ realisation of
// legion://types/EmbodimentPolicy — the typed, AND-intersected policy the
// bootstrapper passes to the one-shot install. install() mounts a capability
// only when the policy allows it; it never widens the policy.

#ifndef AURELIAN_MEMBRANE_EMBODIMENT_POLICY_H_
#define AURELIAN_MEMBRANE_EMBODIMENT_POLICY_H_

#include <map>
#include <set>
#include <string>

namespace aurelian {

class EmbodimentPolicy {
 public:
  EmbodimentPolicy();
  ~EmbodimentPolicy();
  EmbodimentPolicy(const EmbodimentPolicy&);
  EmbodimentPolicy& operator=(const EmbodimentPolicy&);

  // A policy permitting exactly `allowed` (with optional per-capability scope).
  static EmbodimentPolicy WithCapabilities(
      std::set<std::string> allowed,
      std::map<std::string, std::string> scopes = {});

  // Standalone bring-up (Agrippa absent): every capability Aurelian mounts.
  // The typed, sealed, one-shot expression of full trust — NOT an un-typed
  // ambient self-grant.
  static EmbodimentPolicy FullStandalone();

  // policy.allows(name) — name is in allowedCapabilities. Pure read.
  bool Allows(const std::string& name) const;

  // perCapabilityScope[name], or "" if none. Pure read.
  std::string ScopeOf(const std::string& name) const;

  const std::set<std::string>& allowed_capabilities() const {
    return allowed_;
  }

 private:
  std::set<std::string> allowed_;
  std::map<std::string, std::string> scopes_;
};

}  // namespace aurelian

#endif  // AURELIAN_MEMBRANE_EMBODIMENT_POLICY_H_
