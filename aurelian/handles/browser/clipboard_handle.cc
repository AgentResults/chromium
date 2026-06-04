// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/handles/browser/clipboard_handle.h"

#include <optional>

#include "base/functional/bind.h"
#include "base/run_loop.h"
#include "base/strings/utf_string_conversions.h"
#include "ui/base/clipboard/clipboard.h"
#include "ui/base/clipboard/clipboard_buffer.h"
#include "ui/base/clipboard/scoped_clipboard_writer.h"

namespace aurelian {

void WriteClipboardText(const std::string& text) {
  ui::ScopedClipboardWriter writer(ui::ClipboardBuffer::kCopyPaste);
  writer.WriteText(base::UTF8ToUTF16(text));
  // Flushed when `writer` is destroyed.
}

std::string ReadClipboardText() {
  ui::Clipboard* clipboard = ui::Clipboard::GetForCurrentThread();
  if (!clipboard) {
    return std::string();
  }
  std::u16string result;
  base::RunLoop loop;
  clipboard->ReadText(
      ui::ClipboardBuffer::kCopyPaste, /*data_dst=*/std::nullopt,
      base::BindOnce(
          [](std::u16string* out, base::RunLoop* l, std::u16string text) {
            *out = std::move(text);
            l->Quit();
          },
          &result, &loop));
  loop.Run();
  return base::UTF16ToUTF8(result);
}

}  // namespace aurelian
