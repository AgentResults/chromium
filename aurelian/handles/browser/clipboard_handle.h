// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C13.b: clipboard — read / write text (browser handle).

#ifndef AURELIAN_HANDLES_BROWSER_CLIPBOARD_HANDLE_H_
#define AURELIAN_HANDLES_BROWSER_CLIPBOARD_HANDLE_H_

#include <string>

namespace aurelian {

// Writes plain text to the copy/paste clipboard. UI thread.
void WriteClipboardText(const std::string& text);

// Reads plain text from the copy/paste clipboard ("" if none). UI thread.
std::string ReadClipboardText();

// Writes / reads HTML markup to the copy/paste clipboard. UI thread.
void WriteClipboardHtml(const std::string& html);
std::string ReadClipboardHtml();

}  // namespace aurelian

#endif  // AURELIAN_HANDLES_BROWSER_CLIPBOARD_HANDLE_H_
