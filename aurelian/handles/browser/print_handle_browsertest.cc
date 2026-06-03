// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C5.c — print-to-PDF handle
// (legion://chrome/browser/tabs/<id>/print).
//
// RED-first: authored against the unbuilt AurelianPrintHandle.
// ask("printToPdf", {options?}) renders the loaded page to a PDF and
// returns it base64-encoded.

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
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"
#include "velite/agentspaces-wire/handle.hpp"
#include "velite/agentspaces-wire/value_handle.hpp"

namespace aurelian {

using V = velite::agentspaces::Value;
using StateKind = velite::agentspaces::StateKind;

class AurelianPrintBrowserTest : public InProcessBrowserTest {
 protected:
  content::WebContents* GetWC() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }

  std::shared_ptr<velite::agentspaces::Handle>& GetHandle(
      TabHandleImpl* impl) {
    return *static_cast<std::shared_ptr<velite::agentspaces::Handle>*>(
        impl->handle_ptr);
  }

  void NavigateTo(const std::string& html) {
    ASSERT_TRUE(ui_test_utils::NavigateToURL(
        browser(), GURL("data:text/html," + html)));
  }

  void WaitForSettled(std::shared_ptr<velite::agentspaces::Handle> h) {
    if (h->state_kind() != StateKind::Pending) return;
    base::RunLoop run_loop;
    base::RepeatingTimer timer;
    timer.Start(FROM_HERE, base::Milliseconds(10),
                base::BindRepeating(
                    [](base::RunLoop* loop,
                       std::shared_ptr<velite::agentspaces::Handle>* hp) {
                      if ((*hp)->state_kind() != StateKind::Pending)
                        loop->Quit();
                    },
                    &run_loop, &h));
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, run_loop.QuitClosure(), base::Seconds(30));
    run_loop.Run();
    timer.Stop();
  }

  // Returns the base64 PDF, or empty string on failure.
  std::string PrintBase64(velite::agentspaces::Handle* print, const V& spec) {
    auto job = print->ask("printToPdf", spec);
    WaitForSettled(job);
    if (job->state_kind() == StateKind::ResolvedValue &&
        job->resolved_value().is_string()) {
      return job->resolved_value().as_string();
    }
    return "";
  }

  static bool IsPdf(const std::string& b64) {
    std::string bytes;
    if (!base::Base64Decode(b64, &bytes) || bytes.size() < 5)
      return false;
    return bytes.compare(0, 5, "%PDF-") == 0;
  }
};

IN_PROC_BROWSER_TEST_F(AurelianPrintBrowserTest, PrintHandleMounts) {
  NavigateTo("<body>print</body>");
  auto tab = CreateTabHandle(GetWC(), 9201);
  auto& handle = GetHandle(tab.get());

  auto print = handle->ask("print", V());
  ASSERT_EQ(print->state_kind(), StateKind::ResolvedValue);
  ASSERT_TRUE(print->resolved_value().is_string());
  EXPECT_NE(print->resolved_value().as_string().find(
                "legion://chrome/browser/tabs/9201/print"),
            std::string::npos);

  DestroyTabHandle(std::move(tab));
}

IN_PROC_BROWSER_TEST_F(AurelianPrintBrowserTest, PrintsLoadedPage) {
  NavigateTo("<body><h1>Aurelian C5.c</h1><p>hello pdf</p></body>");
  auto tab = CreateTabHandle(GetWC(), 9202);
  auto& handle = GetHandle(tab.get());

  auto print = handle->ask("print", V());
  ASSERT_EQ(print->state_kind(), StateKind::ResolvedValue);

  std::string b64 = PrintBase64(print.get(), V());
  ASSERT_FALSE(b64.empty()) << "print produced no PDF";
  EXPECT_TRUE(IsPdf(b64)) << "decoded bytes are not a PDF";

  DestroyTabHandle(std::move(tab));
}

IN_PROC_BROWSER_TEST_F(AurelianPrintBrowserTest, PrintLandscape) {
  NavigateTo("<body><p>landscape</p></body>");
  auto tab = CreateTabHandle(GetWC(), 9203);
  auto& handle = GetHandle(tab.get());

  auto print = handle->ask("print", V());
  ASSERT_EQ(print->state_kind(), StateKind::ResolvedValue);

  std::string b64 =
      PrintBase64(print.get(), V::make_object({{"landscape", V(true)}}));
  ASSERT_FALSE(b64.empty()) << "landscape print produced no PDF";
  EXPECT_TRUE(IsPdf(b64));

  DestroyTabHandle(std::move(tab));
}

IN_PROC_BROWSER_TEST_F(AurelianPrintBrowserTest, PrintWithMargins) {
  NavigateTo("<body><p>margins</p></body>");
  auto tab = CreateTabHandle(GetWC(), 9204);
  auto& handle = GetHandle(tab.get());

  auto print = handle->ask("print", V());
  ASSERT_EQ(print->state_kind(), StateKind::ResolvedValue);

  std::string b64 = PrintBase64(print.get(), V::make_object({
                                                 {"marginTop", V(0.0)},
                                                 {"marginBottom", V(0.0)},
                                                 {"marginLeft", V(0.0)},
                                                 {"marginRight", V(0.0)},
                                             }));
  ASSERT_FALSE(b64.empty()) << "margin print produced no PDF";
  EXPECT_TRUE(IsPdf(b64));

  DestroyTabHandle(std::move(tab));
}

}  // namespace aurelian
