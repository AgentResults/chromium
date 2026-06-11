// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// CF-0 — vendored conformance bootstrap into //velite (build restore).
//
// RED-first: this test LINKS against the vendored conformance-peer surface
// (make_seeded_bootstrap in peer_session.cpp, the ConformanceBootstrap verb
// table in conformance_bootstrap.cpp/bootstrap_verbs.cpp, and the
// out-of-line CounterHandle::ask_impl bodies in bootstrap_handles.cpp).
// All seven of those wire sources are missing from //velite:velite_wire
// today (the TEMP-WORKAROUND exclusion + the six never-listed splits), so
// the RED is a literal linker failure. Adding the sources makes it GREEN.
// (AURELIAN-CONFORMANT-FEDERATION-TDD-PLAN.md CF-0; design §1.2.)

#include <memory>

#include "testing/gtest/include/gtest/gtest.h"
#include "velite/agentspaces-wire/handle.hpp"
#include "velite/agentspaces-wire/peer_session.hpp"

namespace aurelian {
namespace {

using V = velite::agentspaces::Value;
using Handle = velite::agentspaces::Handle;
using StateKind = velite::agentspaces::StateKind;

TEST(ConformanceBootstrapLinkTest, SeededBootstrapAnswersPing) {
  // Explicit seed: deterministic identity, no environment touched
  // (peer_session.hpp's WASM-safe path).
  auto bootstrap =
      velite::agentspaces::wire::make_seeded_bootstrap("cf0-test-seed");
  ASSERT_TRUE(bootstrap);

  // ping -> resolved "pong" (the corpus fixture verb, bootstrap_verbs.cpp).
  auto pong = bootstrap->ask("ping", V());
  ASSERT_TRUE(pong);
  ASSERT_EQ(pong->state_kind(), StateKind::ResolvedValue);
  ASSERT_TRUE(pong->resolved_value().is_string());
  EXPECT_EQ(pong->resolved_value().as_string(), "pong");

  // getOrgIdentity -> an identity object carrying a non-empty destHash
  // (the seeded Ed25519 keychain identity).
  auto ident = bootstrap->ask("getOrgIdentity", V());
  ASSERT_TRUE(ident);
  ASSERT_EQ(ident->state_kind(), StateKind::ResolvedValue);
  const V* dest_hash = ident->resolved_value().object_get("destHash");
  ASSERT_NE(dest_hash, nullptr);
  ASSERT_TRUE(dest_hash->is_string());
  EXPECT_FALSE(dest_hash->as_string().empty());

  // makeCounter -> a CounterHandle (HandlePromise alias); incr -> 1. This
  // leg links the out-of-line CounterHandle::ask_impl bodies — the seventh
  // missing wire source (bootstrap_handles.cpp), needed by L0-core 05/06.
  auto counter_promise = bootstrap->ask("makeCounter", V());
  ASSERT_TRUE(counter_promise);
  ASSERT_EQ(counter_promise->state_kind(), StateKind::ResolvedHandle);
  std::shared_ptr<Handle> counter = counter_promise->resolved_handle();
  ASSERT_TRUE(counter);
  auto incremented = counter->ask("incr", V());
  ASSERT_TRUE(incremented);
  ASSERT_EQ(incremented->state_kind(), StateKind::ResolvedValue);
  ASSERT_TRUE(incremented->resolved_value().is_int());
  EXPECT_EQ(incremented->resolved_value().as_int(), 1);
}

}  // namespace
}  // namespace aurelian
