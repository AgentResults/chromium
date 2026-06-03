// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/capability/cap_predicate.h"

#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"

namespace aurelian {

CapPredicate::CapPredicate() = default;
CapPredicate::~CapPredicate() = default;
CapPredicate::CapPredicate(const CapPredicate&) = default;
CapPredicate& CapPredicate::operator=(const CapPredicate&) = default;
CapPredicate::CapPredicate(CapPredicate&&) = default;
CapPredicate& CapPredicate::operator=(CapPredicate&&) = default;

namespace {

bool Contains(const std::vector<std::string>& v, const std::string& x) {
  for (const auto& e : v) {
    if (e == x) return true;
  }
  return false;
}

// True if every element of `child` is also in `parent` (child's allowlist is a
// subset — at least as restrictive). An empty list means "any", so an empty
// child under a non-empty parent is a broadening (not a subset).
bool ListIsSubset(const std::vector<std::string>& child,
                  const std::vector<std::string>& parent) {
  if (parent.empty()) return true;       // parent allows any → child always ⊆
  if (child.empty()) return false;       // child allows any, parent restricts
  for (const auto& c : child) {
    if (!Contains(parent, c)) return false;
  }
  return true;
}

}  // namespace

bool GlobMatch(const std::string& pattern, const std::string& value) {
  // Iterative '*' wildcard match (each '*' matches any run, incl. empty).
  size_t p = 0, v = 0;
  size_t star = std::string::npos, mark = 0;
  while (v < value.size()) {
    if (p < pattern.size() && pattern[p] == '*') {
      star = p++;
      mark = v;
    } else if (p < pattern.size() && pattern[p] == value[v]) {
      ++p;
      ++v;
    } else if (star != std::string::npos) {
      p = star + 1;
      v = ++mark;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '*') {
    ++p;
  }
  return p == pattern.size();
}

CapPredicate CapPredicate::Parse(const std::string& body) {
  CapPredicate out;
  for (const std::string& pair : base::SplitString(
           body, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    size_t eq = pair.find('=');
    if (eq == std::string::npos) continue;
    std::string key = pair.substr(0, eq);
    std::string value = pair.substr(eq + 1);
    if (key == "mode") {
      if (value == "read") {
        out.mode = Mode::kRead;
      } else if (value == "write") {
        out.mode = Mode::kWrite;
      }
    } else if (key == "verbs") {
      out.verbs = base::SplitString(value, "|", base::TRIM_WHITESPACE,
                                    base::SPLIT_WANT_NONEMPTY);
    } else if (key == "expires") {
      int64_t ts = 0;
      if (base::StringToInt64(value, &ts)) out.expires = ts;
    } else if (key == "pattern") {
      out.pattern = value;
    } else if (key == "region") {
      out.region = value;
    }
  }
  return out;
}

bool CapPredicate::Allows(const std::string& verb,
                          bool is_mutating,
                          const std::string& target,
                          int64_t now_unix,
                          std::string* reason) const {
  auto deny = [&](const char* r) {
    if (reason) *reason = r;
    return false;
  };
  if (expires != 0 && now_unix > expires) {
    return deny("cap-expired");
  }
  if (!verbs.empty() && !Contains(verbs, verb)) {
    return deny("verb-not-permitted");
  }
  if (mode == Mode::kRead && is_mutating) {
    return deny("mode-read");
  }
  if (!pattern.empty() && !GlobMatch(pattern, target)) {
    return deny("pattern-mismatch");
  }
  if (reason) reason->clear();
  return true;
}

bool CapPredicate::IsReductionOf(const CapPredicate& parent) const {
  // Mode: read is stricter than write/unset; write is stricter than unset. A
  // child may not be less strict than its parent.
  auto strictness = [](Mode m) {
    switch (m) {
      case Mode::kRead:
        return 2;
      case Mode::kWrite:
        return 1;
      case Mode::kUnset:
        return 0;
    }
    return 0;
  };
  if (strictness(mode) < strictness(parent.mode)) {
    return false;
  }
  // Verbs: child's allowlist must be a subset of parent's.
  if (!ListIsSubset(verbs, parent.verbs)) {
    return false;
  }
  // Expiry: child may not outlive parent (0 = never, the broadest).
  if (parent.expires != 0) {
    if (expires == 0 || expires > parent.expires) {
      return false;
    }
  }
  // Pattern: a non-empty parent pattern requires the child to also be scoped,
  // and the child's pattern must match within the parent's (every value the
  // child admits the parent must admit). We approximate strictness by requiring
  // the child's pattern to be matched by the parent's glob.
  if (!parent.pattern.empty()) {
    if (pattern.empty() || !GlobMatch(parent.pattern, pattern)) {
      return false;
    }
  }
  // Region: a non-empty parent region requires an equal child region.
  if (!parent.region.empty() && region != parent.region) {
    return false;
  }
  return true;
}

}  // namespace aurelian
