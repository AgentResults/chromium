// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C10 — asmodeus tab-audio capture, now routed through a Velite handle:
// start / level / stop flow through the handle's ask dispatch on a real tab.

#include "aurelian/handles/profile/audio_capture_handle.h"

#include "base/files/scoped_temp_dir.h"
#include "base/threading/thread_restrictions.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace aurelian {

class AurelianAudioHandleBrowserTest : public InProcessBrowserTest {
 protected:
  content::WebContents* GetWC() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }
};

IN_PROC_BROWSER_TEST_F(AurelianAudioHandleBrowserTest, StartLevelStop) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>audio</title>audio body")));

  // The capture (and temp dir) do blocking WAV file I/O, which the asmodeus
  // path performs on this thread; allow it for the test.
  base::ScopedAllowBlockingForTesting allow_blocking;

  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  std::string wav = dir.GetPath().AppendASCII("cap.wav").AsUTF8Unsafe();

  ScopedAudioCaptureSession session(CreateAudioCaptureHandle(GetWC()));
  ASSERT_NE(session.get(), nullptr);

  // Before start: not capturing.
  double rms = -1, peak = -1;
  bool capturing = true;
  AudioCaptureHandleLevel(session.get(), &rms, &peak, &capturing);
  EXPECT_FALSE(capturing);

  // Start routed through the handle.
  ASSERT_TRUE(AudioCaptureHandleStart(session.get(), wav, 48000, 1));

  // Now capturing — read through the handle.
  AudioCaptureHandleLevel(session.get(), &rms, &peak, &capturing);
  EXPECT_TRUE(capturing);

  // Stop routed through the handle; the WAV path round-trips.
  AudioCaptureStats stats = AudioCaptureHandleStop(session.get());
  EXPECT_EQ(stats.output_path, wav);

  // After stop: not capturing.
  AudioCaptureHandleLevel(session.get(), &rms, &peak, &capturing);
  EXPECT_FALSE(capturing);
}

}  // namespace aurelian
