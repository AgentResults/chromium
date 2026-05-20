// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/asmodeus/meeting_backend.h"

namespace asmodeus {

std::string MeetingBackend::GetSignalingUrl() const { return ""; }
std::string MeetingBackend::GetSignalingHost() const { return ""; }
int MeetingBackend::GetSignalingPort() const { return 0; }

}  // namespace asmodeus
