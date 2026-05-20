// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ASMODEUS_PAGE_CONTROLLER_H_
#define CHROME_BROWSER_ASMODEUS_PAGE_CONTROLLER_H_

#include <string>

namespace content {
class WebContents;
}

namespace asmodeus {

// How to find an element on the page.
enum class FindMethod {
  kText,       // Match by visible text content (exact)
  kTextContains, // Match by text content (substring)
  kAriaLabel,  // Match by aria-label attribute (substring)
  kSelector,   // Match by CSS selector
  kCoords,     // Click at absolute coordinates (x,y encoded in target)
};

// Result of a page interaction.
struct PageResult {
  PageResult();
  ~PageResult();
  PageResult(const PageResult&);
  PageResult& operator=(const PageResult&);

  bool success = false;
  std::string error;      // Error message if !success
  std::string element_tag; // Tag name of matched element
  std::string element_text; // Text content of matched element
  int x = 0, y = 0;       // Coordinates of matched element
};

// General-purpose page interaction controller.
// All operations use trusted browser events — not JS .click() which
// web apps can detect and block.
//
// Usage:
//   PageController page;
//   auto result = page.Click(wc, "Join now", FindMethod::kText);
//   auto result = page.Type(wc, "hello@test.com", "input[type=email]");
//   auto result = page.Read(wc, ".participant-count");
class PageController {
 public:
  PageController();
  ~PageController();

  // Click an element found by the given method.
  PageResult Click(content::WebContents* wc,
                   const std::string& target,
                   FindMethod method);

  // Type text into the currently focused element, or into the element
  // matched by target_selector (if non-empty).
  PageResult Type(content::WebContents* wc,
                  const std::string& text,
                  const std::string& target_selector = "");

  // Read text content of element matched by CSS selector.
  PageResult Read(content::WebContents* wc,
                  const std::string& selector);

  // Read the page title.
  std::string GetTitle(content::WebContents* wc);

  // Read the current URL.
  std::string GetURL(content::WebContents* wc);

  // Build the JavaScript to find an element and return its bounding rect.
  // Pure function — testable without browser.
  static std::string BuildFindElementJS(const std::string& target,
                                         FindMethod method);

  // Build the JavaScript to type text into an element.
  static std::string BuildTypeJS(const std::string& text,
                                  const std::string& selector);

  // Build the JavaScript to read text from an element.
  static std::string BuildReadJS(const std::string& selector);
};

}  // namespace asmodeus

#endif  // CHROME_BROWSER_ASMODEUS_PAGE_CONTROLLER_H_
