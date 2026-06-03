// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C5.b — screenshot handle
// (legion://chrome/browser/tabs/<id>/screenshot).
//
// RED-first: authored against the unbuilt AurelianScreenshotHandle.
// ask("capture") returns a base64 PNG of the tab's compositor surface;
// an unknown format returns broken("unsupported-format").

#include "aurelian/handles/browser/tab_handle.h"

#include "base/base64.h"
#include "base/run_loop.h"
#include "base/task/single_thread_task_runner.h"
#include "base/timer/timer.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"
#include "velite/agentspaces-wire/handle.hpp"
#include "velite/agentspaces-wire/value_handle.hpp"

namespace aurelian {

using V = velite::agentspaces::Value;
using StateKind = velite::agentspaces::StateKind;

class AurelianScreenshotBrowserTest : public InProcessBrowserTest {
 protected:
  void SetUp() override {
    // The compositor must produce real pixels for CopyFromSurface to
    // return a non-empty bitmap.
    EnablePixelOutput();
    InProcessBrowserTest::SetUp();
  }

  content::WebContents* GetWC() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }

  std::shared_ptr<velite::agentspaces::Handle>& GetHandle(
      TabHandleImpl* impl) {
    return *static_cast<std::shared_ptr<velite::agentspaces::Handle>*>(
        impl->handle_ptr);
  }

  void WaitForSettled(std::shared_ptr<velite::agentspaces::Handle> h) {
    if (h->state_kind() != StateKind::Pending) return;
    base::RunLoop run_loop;
    base::RepeatingTimer timer;
    timer.Start(FROM_HERE, base::Milliseconds(5),
                base::BindRepeating(
                    [](base::RunLoop* loop,
                       std::shared_ptr<velite::agentspaces::Handle>* hp) {
                      if ((*hp)->state_kind() != StateKind::Pending)
                        loop->Quit();
                    },
                    &run_loop, &h));
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, run_loop.QuitClosure(), base::Seconds(10));
    run_loop.Run();
    timer.Stop();
  }

  void GiveItSomeTime() {
    base::RunLoop run_loop;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, run_loop.QuitClosure(), base::Milliseconds(50));
    run_loop.Run();
  }

  // Capture, retrying while the surface is not yet ready (broken).
  std::string CaptureBase64(velite::agentspaces::Handle* shot, const V& spec) {
    for (int attempt = 0; attempt < 60; ++attempt) {
      auto cap = shot->ask("capture", spec);
      WaitForSettled(cap);
      if (cap->state_kind() == StateKind::ResolvedValue &&
          cap->resolved_value().is_string()) {
        return cap->resolved_value().as_string();
      }
      GiveItSomeTime();
    }
    return "";
  }

  static bool IsPng(const std::string& b64) {
    std::string bytes;
    if (!base::Base64Decode(b64, &bytes) || bytes.size() < 8)
      return false;
    const unsigned char sig[8] = {0x89, 0x50, 0x4E, 0x47,
                                  0x0D, 0x0A, 0x1A, 0x0A};
    for (int i = 0; i < 8; ++i) {
      if (static_cast<unsigned char>(bytes[i]) != sig[i])
        return false;
    }
    return true;
  }
};

IN_PROC_BROWSER_TEST_F(AurelianScreenshotBrowserTest, ScreenshotHandleMounts) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<body>shot</body>")));
  auto tab = CreateTabHandle(GetWC(), 9101);
  auto& handle = GetHandle(tab.get());

  auto shot = handle->ask("screenshot", V());
  ASSERT_EQ(shot->state_kind(), StateKind::ResolvedValue);
  ASSERT_TRUE(shot->resolved_value().is_string());
  EXPECT_NE(shot->resolved_value().as_string().find(
                "legion://chrome/browser/tabs/9101/screenshot"),
            std::string::npos);

  DestroyTabHandle(std::move(tab));
}

IN_PROC_BROWSER_TEST_F(AurelianScreenshotBrowserTest, CaptureReturnsPng) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(),
      GURL("data:text/html,<body style='background:%233050ff'>x</body>")));
  auto tab = CreateTabHandle(GetWC(), 9102);
  auto& handle = GetHandle(tab.get());

  auto shot = handle->ask("screenshot", V());
  ASSERT_EQ(shot->state_kind(), StateKind::ResolvedValue);

  std::string b64 = CaptureBase64(shot.get(), V());
  ASSERT_FALSE(b64.empty()) << "capture never produced a frame";
  EXPECT_TRUE(IsPng(b64)) << "decoded bytes are not a PNG";

  DestroyTabHandle(std::move(tab));
}

IN_PROC_BROWSER_TEST_F(AurelianScreenshotBrowserTest,
                       CaptureWithClipReturnsPng) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(),
      GURL("data:text/html,<body style='background:%2300a000'>x</body>")));
  auto tab = CreateTabHandle(GetWC(), 9103);
  auto& handle = GetHandle(tab.get());

  auto shot = handle->ask("screenshot", V());
  ASSERT_EQ(shot->state_kind(), StateKind::ResolvedValue);

  V clip = V::make_object({
      {"clip", V::make_object({
                   {"x", V(0)},
                   {"y", V(0)},
                   {"width", V(32)},
                   {"height", V(32)},
               })},
  });
  std::string b64 = CaptureBase64(shot.get(), clip);
  ASSERT_FALSE(b64.empty()) << "clipped capture never produced a frame";
  EXPECT_TRUE(IsPng(b64)) << "clipped capture is not a PNG";

  DestroyTabHandle(std::move(tab));
}

IN_PROC_BROWSER_TEST_F(AurelianScreenshotBrowserTest,
                       UnsupportedFormatReturnsBroken) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<body>fmt</body>")));
  auto tab = CreateTabHandle(GetWC(), 9104);
  auto& handle = GetHandle(tab.get());

  auto shot = handle->ask("screenshot", V());
  ASSERT_EQ(shot->state_kind(), StateKind::ResolvedValue);

  auto cap = shot->ask("capture", V::make_object({{"format", V("jpeg")}}));
  WaitForSettled(cap);
  ASSERT_EQ(cap->state_kind(), StateKind::Broken);
  EXPECT_EQ(std::string(cap->broken_reason()), "unsupported-format");

  DestroyTabHandle(std::move(tab));
}

}  // namespace aurelian
