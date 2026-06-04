// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C7.a: accessibility tree — the semantic page model.

#ifndef AURELIAN_HANDLES_BROWSER_ACCESSIBILITY_HANDLE_H_
#define AURELIAN_HANDLES_BROWSER_ACCESSIBILITY_HANDLE_H_

#include <string>
#include <vector>

namespace content {
class WebContents;
}

namespace aurelian {

// One node of the flattened accessibility tree: its ARIA/platform role and its
// computed accessible name.
struct AxNode {
  std::string role;
  std::string name;
};

// Requests a one-time accessibility snapshot of `wc` (without permanently
// changing the accessibility mode) and returns its nodes flattened to
// {role, name}. Synchronous: must run on the UI thread; spins a RunLoop until
// the renderer replies. Returns empty if the snapshot fails.
std::vector<AxNode> SnapshotAxTree(content::WebContents* wc);

// As SnapshotAxTree, but returns the HIERARCHICAL tree as JSON
// ({"role","name","children":[...]}), preserving parent/child structure.
// Returns "null" if the snapshot fails.
std::string GetAxTreeJson(content::WebContents* wc);

}  // namespace aurelian

#endif  // AURELIAN_HANDLES_BROWSER_ACCESSIBILITY_HANDLE_H_
