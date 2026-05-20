// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/asmodeus/page_controller.h"

#include "testing/gtest/include/gtest/gtest.h"

namespace asmodeus {

TEST(PageControllerTest, BuildFindByTextJS) {
  auto js = PageController::BuildFindElementJS("Join now", FindMethod::kText);
  // Should search for exact text match
  EXPECT_NE(js.find("Join now"), std::string::npos);
  EXPECT_NE(js.find("textContent"), std::string::npos);
  EXPECT_NE(js.find("getBoundingClientRect"), std::string::npos);
  // Should return JSON with x, y, tag
  EXPECT_NE(js.find("JSON.stringify"), std::string::npos);
}

TEST(PageControllerTest, BuildFindByTextContainsJS) {
  auto js = PageController::BuildFindElementJS("verification code",
                                                 FindMethod::kTextContains);
  EXPECT_NE(js.find("includes"), std::string::npos);
  EXPECT_NE(js.find("verification code"), std::string::npos);
}

TEST(PageControllerTest, BuildFindByAriaLabelJS) {
  auto js = PageController::BuildFindElementJS("microphone",
                                                 FindMethod::kAriaLabel);
  EXPECT_NE(js.find("aria-label"), std::string::npos);
  EXPECT_NE(js.find("microphone"), std::string::npos);
  EXPECT_NE(js.find("toLowerCase"), std::string::npos);
}

TEST(PageControllerTest, BuildFindBySelectorJS) {
  auto js = PageController::BuildFindElementJS("button.primary",
                                                 FindMethod::kSelector);
  EXPECT_NE(js.find("querySelector"), std::string::npos);
  EXPECT_NE(js.find("button.primary"), std::string::npos);
}

TEST(PageControllerTest, BuildTypeJSWithSelector) {
  auto js = PageController::BuildTypeJS("hello@test.com", "input[type=email]");
  EXPECT_NE(js.find("hello@test.com"), std::string::npos);
  EXPECT_NE(js.find("input[type=email]"), std::string::npos);
  EXPECT_NE(js.find("focus"), std::string::npos);
  EXPECT_NE(js.find("dispatchEvent"), std::string::npos);
}

TEST(PageControllerTest, BuildTypeJSWithoutSelector) {
  auto js = PageController::BuildTypeJS("some text", "");
  EXPECT_NE(js.find("some text"), std::string::npos);
  EXPECT_NE(js.find("activeElement"), std::string::npos);
}

TEST(PageControllerTest, BuildReadJS) {
  auto js = PageController::BuildReadJS(".participant-count");
  EXPECT_NE(js.find("querySelector"), std::string::npos);
  EXPECT_NE(js.find(".participant-count"), std::string::npos);
  EXPECT_NE(js.find("textContent"), std::string::npos);
}

TEST(PageControllerTest, EscapesSpecialChars) {
  auto js = PageController::BuildFindElementJS("it's a \"test\"",
                                                 FindMethod::kText);
  // Should contain escaped quotes
  EXPECT_NE(js.find("\\'"), std::string::npos);
  EXPECT_NE(js.find("\\\""), std::string::npos);
  // Should NOT contain raw unescaped quotes that break JS
  // (The function wraps in single quotes, so ' must be escaped)
}

TEST(PageControllerTest, NullResultForCoords) {
  auto js = PageController::BuildFindElementJS("100,200",
                                                 FindMethod::kCoords);
  EXPECT_NE(js.find("100,200"), std::string::npos);
}

TEST(PageControllerTest, GetTitleReturnsEmptyForNull) {
  PageController ctrl;
  EXPECT_EQ(ctrl.GetTitle(nullptr), "");
  EXPECT_EQ(ctrl.GetURL(nullptr), "");
}

}  // namespace asmodeus
