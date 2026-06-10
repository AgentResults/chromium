// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// HS-1 (ACM-2, AURELIAN-GENERIC-CONTROL-DESIGN section 3): the ONE
// serve-thread blocking dispatch both wire bridge sites consume (the UDS
// register-in lambda and the WSS scaffold's DispatchChromeRoot). Replaces
// the two inline post-and-block-a-WaitableEvent sites: the posted UI task
// now STARTS the command and returns; the serve thread waits on its
// completion record (created pre-post, so Stop()-coverable), and the HS-1
// session layer delivers settlement.

#ifndef AURELIAN_FEDERATION_BRIDGE_DISPATCH_H_
#define AURELIAN_FEDERATION_BRIDGE_DISPATCH_H_

#include <string>

#include "base/time/time.h"

namespace aurelian {

struct ChromeRoot;

// Blocking dispatch from a wire serve thread (the production wait budget).
// On the UI thread itself an already-settled answer returns directly; a
// Pending one is refused typed — a UI-thread caller cannot block on
// settlement without nesting a UI loop, the exact shape HS-1 deletes.
// `serialized_spec` (HS-3, ACM-2w): the caller's spec in canonical-JSON
// form, passed to the final hop only (empty = no spec).
std::string BridgeDispatch(ChromeRoot* root,
                           const std::string& path,
                           const std::string& serialized_spec = {});

// Same mechanism, caller-chosen wait budget (the timeout pin's seam).
std::string BridgeDispatchWithTimeout(ChromeRoot* root,
                                      const std::string& path,
                                      const std::string& serialized_spec,
                                      base::TimeDelta timeout);

}  // namespace aurelian

#endif  // AURELIAN_FEDERATION_BRIDGE_DISPATCH_H_
