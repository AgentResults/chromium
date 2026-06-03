// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/capability/cap_wire.h"

#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"

namespace aurelian {

namespace {

constexpr char kFieldSep = '\x1f';   // between a link's fields
constexpr char kRecordSep = '\x1e';  // between links
constexpr char kCapMarker = '\x01';  // envelope: cap present
constexpr char kCapEnd = '\x02';     // envelope: end of cap, start of body

template <size_t N>
bool HexInto(const std::string& hex, std::array<uint8_t, N>* out) {
  std::vector<uint8_t> bytes;
  if (!base::HexStringToBytes(hex, &bytes) || bytes.size() != N) {
    return false;
  }
  std::copy(bytes.begin(), bytes.end(), out->begin());
  return true;
}

}  // namespace

std::string SerializeChain(const std::vector<CapLink>& chain) {
  std::string out;
  for (size_t i = 0; i < chain.size(); ++i) {
    if (i > 0) out.push_back(kRecordSep);
    const CapLink& l = chain[i];
    out += l.cap_id;
    out.push_back(kFieldSep);
    out += l.parent_cap_id;
    out.push_back(kFieldSep);
    out += l.predicate;
    out.push_back(kFieldSep);
    out += base::NumberToString(l.expires);
    out.push_back(kFieldSep);
    out += base::HexEncode(l.issuer_pub);
    out.push_back(kFieldSep);
    out += base::HexEncode(l.subject_pub);
    out.push_back(kFieldSep);
    out += base::HexEncode(l.signature);
  }
  return out;
}

bool ParseChain(const std::string& s, std::vector<CapLink>* out) {
  out->clear();
  if (s.empty()) return false;
  for (const std::string& rec : base::SplitStringUsingSubstr(
           s, std::string(1, kRecordSep), base::KEEP_WHITESPACE,
           base::SPLIT_WANT_ALL)) {
    std::vector<std::string> f = base::SplitStringUsingSubstr(
        rec, std::string(1, kFieldSep), base::KEEP_WHITESPACE,
        base::SPLIT_WANT_ALL);
    if (f.size() != 7) return false;
    CapLink l;
    l.cap_id = f[0];
    l.parent_cap_id = f[1];
    l.predicate = f[2];
    if (!base::StringToInt64(f[3], &l.expires)) return false;
    if (!HexInto(f[4], &l.issuer_pub)) return false;
    if (!HexInto(f[5], &l.subject_pub)) return false;
    if (!HexInto(f[6], &l.signature)) return false;
    out->push_back(std::move(l));
  }
  return true;
}

std::vector<uint8_t> EncodeCapEnvelope(const std::vector<CapLink>& chain,
                                       const std::string& verb,
                                       const std::string& param) {
  std::string body = param.empty() ? verb : verb + "\t" + param;
  std::string s;
  s.push_back(kCapMarker);
  s += SerializeChain(chain);
  s.push_back(kCapEnd);
  s += body;
  return std::vector<uint8_t>(s.begin(), s.end());
}

bool DecodeEnvelope(const std::vector<uint8_t>& envelope,
                    bool* has_cap,
                    std::vector<CapLink>* chain,
                    std::string* rest) {
  *has_cap = false;
  chain->clear();
  std::string s(envelope.begin(), envelope.end());
  if (s.empty() || s[0] != kCapMarker) {
    *rest = s;  // plain envelope, no cap
    return true;
  }
  size_t end = s.find(kCapEnd, 1);
  if (end == std::string::npos) {
    *rest = std::string();
    return false;
  }
  std::string chain_str = s.substr(1, end - 1);
  *rest = s.substr(end + 1);
  if (!ParseChain(chain_str, chain)) {
    return false;
  }
  *has_cap = true;
  return true;
}

}  // namespace aurelian
