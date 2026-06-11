// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// CF-1 (conformant-federation design §4.1): the navigable chrome-root Handle
// factory, INTERNAL to the federation/conformance layer. The public
// uds_register.h stays velite-free per the pimpl discipline; this header is
// velite-typed and consumed only by velite-aware TUs (the conformance serve
// mounts the returned handle at legion://chrome through the vendored
// bootstrap's mount_resource — the Marius embodiment-install primitive).
// ONE NavHandle implementation (uds_register.cc) serves both bring-ups.

#ifndef AURELIAN_FEDERATION_NAV_HANDLE_INTERNAL_H_
#define AURELIAN_FEDERATION_NAV_HANDLE_INTERNAL_H_

#include <memory>
#include <string>

#include "aurelian/federation/uds_register.h"
#include "velite/agentspaces-wire/handle.hpp"

namespace aurelian {

// A navigable NavHandle bound to `uri`, resolving leaf asks through
// `dispatch` (the ONE exported ChromeDispatchFn; HS-3 serialized value-only
// spec). Deeper getResource chains compose as on the register-in wire.
std::shared_ptr<velite::agentspaces::Handle> MakeChromeNavHandle(
    const std::string& uri, ChromeDispatchFn dispatch);

}  // namespace aurelian

#endif  // AURELIAN_FEDERATION_NAV_HANDLE_INTERNAL_H_
