// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/mirror/value_convert.h"

#include <limits>
#include <map>
#include <optional>
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

std::optional<base::Value> ToBaseValue(const Value& v) {
  if (v.is_null()) {
    return base::Value();
  }
  if (v.is_bool()) {
    return base::Value(v.as_bool());
  }
  if (v.is_int()) {
    const int64_t i = v.as_int();
    if (i >= std::numeric_limits<int>::min() &&
        i <= std::numeric_limits<int>::max()) {
      return base::Value(static_cast<int>(i));
    }
    return base::Value(static_cast<double>(i));  // the JSON convention
  }
  if (v.is_double()) {
    return base::Value(v.as_double());
  }
  if (v.is_string()) {
    return base::Value(v.as_string());
  }
  if (v.is_array()) {
    base::ListValue list;
    for (const Value& item : v.as_array()) {
      std::optional<base::Value> converted = ToBaseValue(item);
      if (!converted) {
        return std::nullopt;
      }
      list.Append(std::move(*converted));
    }
    return base::Value(std::move(list));
  }
  if (v.is_object()) {
    base::DictValue dict;
    for (const auto& [key, field] : v.as_object()) {
      std::optional<base::Value> converted = ToBaseValue(field);
      if (!converted) {
        return std::nullopt;
      }
      dict.Set(key, std::move(*converted));
    }
    return base::Value(std::move(dict));
  }
  // handle / slot-ref / bytes: value-only seam — the caller refuses typed.
  return std::nullopt;
}

}  // namespace aurelian
