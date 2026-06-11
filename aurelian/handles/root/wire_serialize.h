// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian: the federation-wire reply serializer. The bridge dispatch ->
// RootDispatch asks a leaf handle and serializes its reply to a transportable
// string with this. Top-level string/int replies keep their bare scalar form
// (the wire's "verb -> scalar" contract callers parse directly); every richer
// value (objects, arrays, bools, doubles, nested) is canonical JSON via the
// shared Velite marshaller, so no value kind is silently dropped on the wire.

#ifndef AURELIAN_HANDLES_ROOT_WIRE_SERIALIZE_H_
#define AURELIAN_HANDLES_ROOT_WIRE_SERIALIZE_H_

#include <memory>
#include <string>

#include "velite/agentspaces-wire/handle.hpp"

namespace aurelian {

// Serializes a settled handle's reply for the federation wire. A broken handle
// becomes "broken:<reason>"; a null handle "broken:null".
std::string SerializeWireReply(
    const std::shared_ptr<velite::agentspaces::Handle>& h);

}  // namespace aurelian

#endif  // AURELIAN_HANDLES_ROOT_WIRE_SERIALIZE_H_
