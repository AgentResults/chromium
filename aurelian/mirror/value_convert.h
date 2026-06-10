// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Lossless base::Value -> velite Value conversion, shared by the mirror's
// descriptor projection (ACM-1) and the session layer's CDP reply parsing
// (ACM-2). One converter — parsed JSON only (null/bool/int/double/string/
// list/dict), so every kind converts and nothing narrows silently.

#ifndef AURELIAN_MIRROR_VALUE_CONVERT_H_
#define AURELIAN_MIRROR_VALUE_CONVERT_H_

#include "base/values.h"

namespace velite::agentspaces {
class Value;
}

namespace aurelian {

velite::agentspaces::Value FromBaseValue(const base::Value& v);
velite::agentspaces::Value FromBaseDict(const base::DictValue& d);

}  // namespace aurelian

#endif  // AURELIAN_MIRROR_VALUE_CONVERT_H_
