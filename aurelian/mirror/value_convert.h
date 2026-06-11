// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Lossless base::Value <-> velite Value conversion, shared by the mirror's
// descriptor projection (ACM-1), the session layer's CDP reply parsing
// (ACM-2), and the prefs mirror's write path (ACM-5). One converter each
// way — parsed-JSON kinds only (null/bool/int/double/string/list/dict), so
// every kind converts and nothing narrows silently.

#ifndef AURELIAN_MIRROR_VALUE_CONVERT_H_
#define AURELIAN_MIRROR_VALUE_CONVERT_H_

#include <optional>

#include "base/values.h"

namespace velite::agentspaces {
class Value;
}

namespace aurelian {

velite::agentspaces::Value FromBaseValue(const base::Value& v);
velite::agentspaces::Value FromBaseDict(const base::DictValue& d);

// ACM-5: the reverse direction, VALUE-ONLY (the lossless-or-refuse
// doctrine): a handle-, slot-ref- or bytes-bearing Value answers nullopt —
// the caller refuses typed, never silently flattens. An int64 outside
// base::Value's int32 range converts to double (the JSON convention).
std::optional<base::Value> ToBaseValue(const velite::agentspaces::Value& v);

}  // namespace aurelian

#endif  // AURELIAN_MIRROR_VALUE_CONVERT_H_
