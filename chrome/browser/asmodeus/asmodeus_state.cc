// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/asmodeus/asmodeus_state.h"

#include <atomic>

namespace asmodeus {

namespace {
std::atomic<bool> g_suppressed{false};
}  // namespace

bool IsSuppressed() {
  return g_suppressed.load(std::memory_order_relaxed);
}

void SetSuppressed(bool suppressed) {
  g_suppressed.store(suppressed, std::memory_order_relaxed);
}

}  // namespace asmodeus
