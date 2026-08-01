// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian: the federation-wire reply serializer. The bridge dispatch ->
// RootDispatch asks a leaf handle and renders its settled reply into a
// WireReply — the answer as canonical JSON, or a typed refusal — which the
// dispatch handle then maps back onto the substrate Handle lifecycle.
//
// Two properties this seam owes the federation:
//   * A refusal stays a REFUSAL. It comes back Kind::kBroken and settles the
//     dispatch handle Broken, never a ResolvedValue holding "broken:<reason>".
//   * A value keeps its TYPE. Every kind — scalars included — is canonical
//     JSON, so the receiver restores the Value losslessly instead of
//     re-parsing a string and guessing.

#ifndef AURELIAN_HANDLES_ROOT_WIRE_SERIALIZE_H_
#define AURELIAN_HANDLES_ROOT_WIRE_SERIALIZE_H_

#include <memory>

#include "aurelian/handles/root/wire_reply.h"
#include "velite/agentspaces-wire/handle.hpp"

namespace aurelian {

// Renders a settled handle's reply for the federation wire.
//
// Broken           -> Kind::kBroken, the handle's reason verbatim.
// null handle      -> Kind::kBroken, "null-answer".
// Pending          -> Kind::kBroken, "answer-still-pending". The caller owns
//                     the wait (RootDispatch returns Pending answers to it);
//                     reaching here means that contract was broken, which is
//                     reported, never papered over with an empty value.
// ResolvedHandle   -> Kind::kBroken, "handle-answer-not-serializable". A
//                     handle answer needs a slot table this seam does not
//                     have; lowering it to `null` would be a silent drop.
// ResolvedValue    -> SerializeWireValue of the resolved value.
WireReply SerializeWireReply(
    const std::shared_ptr<velite::agentspaces::Handle>& h);

// Renders a Value as canonical JSON, or refuses with "conversion-lossy" when
// the Value carries a kind JSON cannot represent (an in-process handle, a
// wire slot ref, a byte string). The shared marshaller renders those as
// `null`; emitting that as the answer would be a silent drop at the wire.
WireReply SerializeWireValue(const velite::agentspaces::Value& v);

}  // namespace aurelian

#endif  // AURELIAN_HANDLES_ROOT_WIRE_SERIALIZE_H_
