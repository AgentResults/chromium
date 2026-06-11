// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// ACM-5 (AURELIAN-GENERIC-CONTROL-DESIGN sections 2/4): the preferences
// mirror — legion://chrome/prefs — projected from PrefService's OWN
// registry on the live profile (names, types, defaults are the host's;
// nothing hand-authored). prefs/<name> answers get / set; a set is gated
// by the registry's type (the host's write path CHECK-fails on mismatch,
// so the mirror refuses typed BEFORE the host). Mounted behind the sealed
// root when the EmbodimentPolicy grants `prefs`.

#ifndef AURELIAN_MIRROR_PREFS_MIRROR_H_
#define AURELIAN_MIRROR_PREFS_MIRROR_H_

#include <memory>

namespace velite::agentspaces {
class Handle;
}

namespace aurelian {

// The ONE prefs-mirror root node (legion://chrome/prefs). Children are
// the live profile's registered preferences.
std::shared_ptr<velite::agentspaces::Handle> CreatePrefsMirror();

}  // namespace aurelian

#endif  // AURELIAN_MIRROR_PREFS_MIRROR_H_
