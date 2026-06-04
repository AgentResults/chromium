// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian: the navigable legion://chrome/ root handle. It answers identity and
// mounts sub-handles (e.g. /system) that reach real browser capabilities, so a
// caller can walk down the tree from the root — the design's "one model,
// reachable from the root". The C-API hides Velite types from Chrome/test TUs;
// RootDispatch walks a slash-path (e.g. "system/info") and serializes the leaf.

#ifndef AURELIAN_HANDLES_ROOT_ROOT_HANDLE_H_
#define AURELIAN_HANDLES_ROOT_ROOT_HANDLE_H_

#include <string>

namespace aurelian {

class EmbodimentPolicy;

// Opaque owner of the Velite root handle (defined in the .cc).
struct ChromeRoot;

// Full-standalone root (every capability mounted). Equivalent to
// CreateChromeRootWithPolicy(EmbodimentPolicy::FullStandalone()).
ChromeRoot* CreateChromeRoot();

// Policy-gated root: a capability child is reachable only when the policy
// allows it; an unauthorised name resolves broken("out-of-scope"). This is the
// install-membrane seal (AURELIAN-DESIGN.md §4.5).
ChromeRoot* CreateChromeRootWithPolicy(const EmbodimentPolicy& policy);

void DestroyChromeRoot(ChromeRoot* root);

// Walks `path` (segments separated by '/') from the root: each non-final
// segment must resolve to a child handle; the final segment's reply is
// serialized to a string. Examples: "__getIdentity", "system/info". UI thread.
std::string RootDispatch(ChromeRoot* root, const std::string& path);

}  // namespace aurelian

#endif  // AURELIAN_HANDLES_ROOT_ROOT_HANDLE_H_
