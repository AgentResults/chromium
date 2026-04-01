// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ASMODEUS_ASMODEUS_STATE_H_
#define CHROME_BROWSER_ASMODEUS_ASMODEUS_STATE_H_

// Global Asmodeus state flags. Checked by various Chrome subsystems
// to suppress browser UI when the agent is in control.
//
// This is intentionally a simple global rather than a service/singleton
// because it needs to be checked from hot paths (password manager,
// infobar creation, etc.) without dependency injection overhead.

namespace asmodeus {

// Returns true if Asmodeus fingerprint suppression is active.
// When true, browser UI elements that reveal automation should be hidden:
// - Password save/update prompts
// - Password breach warnings
// - Debugger infobar
// - "Save password?" bubble
bool IsSuppressed();

// Set the suppression state. Called by AsmodeusHandler::SuppressFingerprints.
void SetSuppressed(bool suppressed);

}  // namespace asmodeus

#endif  // CHROME_BROWSER_ASMODEUS_ASMODEUS_STATE_H_
