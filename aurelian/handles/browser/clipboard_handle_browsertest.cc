// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C13.b browser tests — clipboard read / write text.

#include "aurelian/handles/browser/clipboard_handle.h"

#include "chrome/test/base/in_process_browser_test.h"
#include "content/public/test/browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/base/clipboard/clipboard.h"
#include "ui/base/clipboard/test/test_clipboard.h"

namespace aurelian {

class AurelianClipboardBrowserTest : public InProcessBrowserTest {
 protected:
  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    // Isolate from the real system clipboard.
    ui::TestClipboard::CreateForCurrentThread();
  }
  void TearDownOnMainThread() override {
    ui::Clipboard::DestroyClipboardForCurrentThread();
    InProcessBrowserTest::TearDownOnMainThread();
  }
};

IN_PROC_BROWSER_TEST_F(AurelianClipboardBrowserTest, WriteRead) {
  WriteClipboardText("aurelian-clip-42");
  EXPECT_EQ(ReadClipboardText(), "aurelian-clip-42");

  // Overwriting replaces the contents.
  WriteClipboardText("second");
  EXPECT_EQ(ReadClipboardText(), "second");
}

IN_PROC_BROWSER_TEST_F(AurelianClipboardBrowserTest, WriteReadHtml) {
  WriteClipboardHtml("<b>aurelian-html</b>");
  EXPECT_NE(ReadClipboardHtml().find("<b>aurelian-html</b>"),
            std::string::npos);
}

IN_PROC_BROWSER_TEST_F(AurelianClipboardBrowserTest, Clear) {
  WriteClipboardText("to-be-cleared");
  ASSERT_EQ(ReadClipboardText(), "to-be-cleared");

  ClearClipboard();
  EXPECT_EQ(ReadClipboardText(), "");
}

}  // namespace aurelian
