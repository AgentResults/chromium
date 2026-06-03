// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C8.c: cap-token wire encoding. A cap-carrying Dispatch envelope
// prefixes the serialized delegation chain so the receiving membrane can verify
// it independently. Shared by the browser (mint/encode) and renderer (decode/
// verify) sides.

#ifndef AURELIAN_CAPABILITY_CAP_WIRE_H_
#define AURELIAN_CAPABILITY_CAP_WIRE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "aurelian/capability/cap_chain.h"

namespace aurelian {

// Serializes a delegation chain to a transportable string and back.
std::string SerializeChain(const std::vector<CapLink>& chain);
bool ParseChain(const std::string& s, std::vector<CapLink>* out);

// Builds a cap-carrying Dispatch envelope: 0x01 + serialized-chain + 0x02 +
// "verb\tparam". A plain "verb\tparam" envelope (no 0x01 prefix) carries no cap.
std::vector<uint8_t> EncodeCapEnvelope(const std::vector<CapLink>& chain,
                                       const std::string& verb,
                                       const std::string& param);

// Decodes an envelope. Sets *has_cap and (if so) fills *chain; *rest always
// receives the "verb\tparam" body. Returns false only if a cap prefix is
// present but malformed.
bool DecodeEnvelope(const std::vector<uint8_t>& envelope,
                    bool* has_cap,
                    std::vector<CapLink>* chain,
                    std::string* rest);

}  // namespace aurelian

#endif  // AURELIAN_CAPABILITY_CAP_WIRE_H_
