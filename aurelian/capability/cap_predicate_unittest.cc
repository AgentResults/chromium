// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/capability/cap_predicate.h"

#include "testing/gtest/include/gtest/gtest.h"

namespace aurelian {
namespace {

TEST(AurelianCapPredicateTest, ParsesAllDimensions) {
  CapPredicate p = CapPredicate::Parse(
      "mode=read,verbs=text|html,expires=1700000000,pattern=*.example.com,"
      "region=us");
  EXPECT_EQ(p.mode, CapPredicate::Mode::kRead);
  ASSERT_EQ(p.verbs.size(), 2u);
  EXPECT_EQ(p.verbs[0], "text");
  EXPECT_EQ(p.verbs[1], "html");
  EXPECT_EQ(p.expires, 1700000000);
  EXPECT_EQ(p.pattern, "*.example.com");
  EXPECT_EQ(p.region, "us");
}

TEST(AurelianCapPredicateTest, GlobMatch) {
  EXPECT_TRUE(GlobMatch("*.example.com", "a.example.com"));
  EXPECT_TRUE(GlobMatch("https://*/path", "https://host/path"));
  EXPECT_TRUE(GlobMatch("*", "anything"));
  EXPECT_TRUE(GlobMatch("exact", "exact"));
  EXPECT_FALSE(GlobMatch("*.example.com", "evil.com"));
  EXPECT_FALSE(GlobMatch("exact", "exactly"));
}

TEST(AurelianCapPredicateTest, ReadModeDeniesMutation) {
  CapPredicate p = CapPredicate::Parse("mode=read");
  std::string reason;
  EXPECT_FALSE(p.Allows("setText", /*is_mutating=*/true, "t", 0, &reason));
  EXPECT_EQ(reason, "mode-read");
  EXPECT_TRUE(p.Allows("text", /*is_mutating=*/false, "t", 0, &reason));
}

TEST(AurelianCapPredicateTest, VerbAllowlist) {
  CapPredicate p = CapPredicate::Parse("verbs=loadUrl|reload");
  std::string reason;
  EXPECT_TRUE(p.Allows("loadUrl", false, "t", 0, &reason));
  EXPECT_TRUE(p.Allows("reload", false, "t", 0, &reason));
  EXPECT_FALSE(p.Allows("close", false, "t", 0, &reason));
  EXPECT_EQ(reason, "verb-not-permitted");
}

TEST(AurelianCapPredicateTest, ExpiryEnforced) {
  CapPredicate p = CapPredicate::Parse("expires=100");
  std::string reason;
  EXPECT_TRUE(p.Allows("text", false, "t", /*now=*/50, &reason));
  EXPECT_FALSE(p.Allows("text", false, "t", /*now=*/150, &reason));
  EXPECT_EQ(reason, "cap-expired");
}

TEST(AurelianCapPredicateTest, PatternEnforced) {
  CapPredicate p = CapPredicate::Parse("pattern=https://*.example.com/*");
  std::string reason;
  EXPECT_TRUE(
      p.Allows("text", false, "https://a.example.com/x", 0, &reason));
  EXPECT_FALSE(p.Allows("text", false, "https://evil.com/x", 0, &reason));
  EXPECT_EQ(reason, "pattern-mismatch");
}

TEST(AurelianCapPredicateTest, ReductionOf) {
  CapPredicate parent = CapPredicate::Parse("mode=write");  // broad
  CapPredicate child = CapPredicate::Parse("mode=read,verbs=text");
  EXPECT_TRUE(child.IsReductionOf(parent));
  // A child that broadens (write under a read parent) is NOT a reduction.
  EXPECT_FALSE(parent.IsReductionOf(child));
}

}  // namespace
}  // namespace aurelian
