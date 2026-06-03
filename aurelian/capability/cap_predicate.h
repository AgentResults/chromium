// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C8.a: cap-token predicates — the SHAPE half of attenuation
// (mode / verbs / expires / pattern / region), ported from the VSCode V7
// predicate vocabulary. Pure value type; no crypto (the signed chain is
// C8.b, cap_chain.h).

#ifndef AURELIAN_CAPABILITY_CAP_PREDICATE_H_
#define AURELIAN_CAPABILITY_CAP_PREDICATE_H_

#include <cstdint>
#include <string>
#include <vector>

namespace aurelian {

// A parsed predicate set, e.g. from
//   legion://chrome/.../dom[mode=read,verbs=text|html,expires=1700000000]
// The bracketed body is "key=value" pairs separated by commas; list values use
// '|' as the inner separator.
struct CapPredicate {
  enum class Mode { kUnset, kRead, kWrite };

  CapPredicate();
  ~CapPredicate();
  CapPredicate(const CapPredicate&);
  CapPredicate& operator=(const CapPredicate&);
  CapPredicate(CapPredicate&&);
  CapPredicate& operator=(CapPredicate&&);

  Mode mode = Mode::kUnset;
  std::vector<std::string> verbs;  // empty = any verb permitted
  int64_t expires = 0;             // unix seconds; 0 = never expires
  std::string pattern;             // glob over the target URI; empty = any
  std::string region;              // empty = any region

  // Parses the bracketed predicate body (without the surrounding []). Unknown
  // keys are ignored. A malformed pair is skipped.
  static CapPredicate Parse(const std::string& body);

  // True if this predicate permits `verb` (a `is_mutating` verb is a write) on
  // `target` at `now_unix`. On denial, writes a reason into `*reason` (one of
  // the §12 cap reasons) and returns false.
  bool Allows(const std::string& verb,
              bool is_mutating,
              const std::string& target,
              int64_t now_unix,
              std::string* reason) const;

  // True if this predicate is a strict reduction of (no broader than) `parent`
  // — every dimension at least as restrictive. Used by the chain verifier to
  // reject a broadening delegation link.
  bool IsReductionOf(const CapPredicate& parent) const;
};

// Glob match supporting '*' (matches any run, including empty). Exposed for the
// chain verifier + tests.
bool GlobMatch(const std::string& pattern, const std::string& value);

}  // namespace aurelian

#endif  // AURELIAN_CAPABILITY_CAP_PREDICATE_H_
