// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/mirror/value_convert.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "velite/agentspaces-wire/handle.hpp"

namespace aurelian {

using velite::agentspaces::Value;

Value FromBaseDict(const base::DictValue& d) {
  std::map<std::string, Value> fields;
  for (const auto [key, field] : d) {
    fields.emplace(key, FromBaseValue(field));
  }
  return Value::make_object(std::move(fields));
}

Value FromBaseValue(const base::Value& v) {
  switch (v.type()) {
    case base::Value::Type::NONE:
      return Value();
    case base::Value::Type::BOOLEAN:
      return Value(v.GetBool());
    case base::Value::Type::INTEGER:
      return Value(v.GetInt());
    case base::Value::Type::DOUBLE:
      return Value(v.GetDouble());
    case base::Value::Type::STRING:
      return Value(v.GetString());
    case base::Value::Type::LIST: {
      std::vector<Value> items;
      for (const base::Value& item : v.GetList()) {
        items.push_back(FromBaseValue(item));
      }
      return Value::make_array(std::move(items));
    }
    case base::Value::Type::DICT:
      return FromBaseDict(v.GetDict());
    case base::Value::Type::BINARY:
      // Unreachable for parsed JSON; refuse loudly rather than narrow.
      return Value();
  }
  return Value();
}

}  // namespace aurelian
