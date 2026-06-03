// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C5.d — find-in-page handle
// (legion://chrome/browser/tabs/<id>/find).
//
// RED-first: authored against the unbuilt AurelianFindHandle.
// ask("find", {query, options?}) returns {matchCount, currentMatch,
// selectionRect}; ask("findNext")/ask("findPrev") step through matches.

#include "aurelian/handles/browser/tab_handle.h"

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

class AurelianFindBrowserTest : public InProcessBrowserTest {
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
        FROM_HERE, run_loop.QuitClosure(), base::Seconds(15));
    run_loop.Run();
    timer.Stop();
  }

  // Issue a find verb and return the settled result object (or null V).
  V FindResult(velite::agentspaces::Handle* find, const std::string& verb,
               const V& spec) {
    auto job = find->ask(verb, spec);
    WaitForSettled(job);
    if (job->state_kind() == StateKind::ResolvedValue)
      return job->resolved_value();
    return V();
  }

  int64_t MatchCount(const V& result) {
    const V* v = result.object_get("matchCount");
    return (v && v->is_int()) ? v->as_int() : -1;
  }
  int64_t CurrentMatch(const V& result) {
    const V* v = result.object_get("currentMatch");
    return (v && v->is_int()) ? v->as_int() : -1;
  }
};

IN_PROC_BROWSER_TEST_F(AurelianFindBrowserTest, FindHandleMounts) {
  NavigateTo("<body>find</body>");
  auto tab = CreateTabHandle(GetWC(), 9301);
  auto& handle = GetHandle(tab.get());

  auto find = handle->ask("find", V());
  ASSERT_EQ(find->state_kind(), StateKind::ResolvedValue);
  ASSERT_TRUE(find->resolved_value().is_string());
  EXPECT_NE(find->resolved_value().as_string().find(
                "legion://chrome/browser/tabs/9301/find"),
            std::string::npos);

  DestroyTabHandle(std::move(tab));
}

IN_PROC_BROWSER_TEST_F(AurelianFindBrowserTest, LocatesMatches) {
  NavigateTo("<body>needle haystack needle straw needle</body>");
  auto tab = CreateTabHandle(GetWC(), 9302);
  auto& handle = GetHandle(tab.get());

  auto find = handle->ask("find", V());
  ASSERT_EQ(find->state_kind(), StateKind::ResolvedValue);

  V result = FindResult(find.get(), "find",
                        V::make_object({{"query", V("needle")}}));
  EXPECT_EQ(MatchCount(result), 3) << "expected 3 matches";

  DestroyTabHandle(std::move(tab));
}

IN_PROC_BROWSER_TEST_F(AurelianFindBrowserTest, FindNextAdvances) {
  NavigateTo("<body>alpha alpha alpha alpha</body>");
  auto tab = CreateTabHandle(GetWC(), 9303);
  auto& handle = GetHandle(tab.get());

  auto find = handle->ask("find", V());
  ASSERT_EQ(find->state_kind(), StateKind::ResolvedValue);

  V first = FindResult(find.get(), "find",
                       V::make_object({{"query", V("alpha")}}));
  EXPECT_EQ(MatchCount(first), 4);
  EXPECT_EQ(CurrentMatch(first), 1) << "first match is ordinal 1";

  V next = FindResult(find.get(), "findNext", V());
  EXPECT_EQ(CurrentMatch(next), 2) << "findNext advances to ordinal 2";

  DestroyTabHandle(std::move(tab));
}

IN_PROC_BROWSER_TEST_F(AurelianFindBrowserTest, FindEmptyReturnsZero) {
  NavigateTo("<body>nothing to see here</body>");
  auto tab = CreateTabHandle(GetWC(), 9304);
  auto& handle = GetHandle(tab.get());

  auto find = handle->ask("find", V());
  ASSERT_EQ(find->state_kind(), StateKind::ResolvedValue);

  V result = FindResult(find.get(), "find",
                        V::make_object({{"query", V("zqxjwk")}}));
  EXPECT_EQ(MatchCount(result), 0) << "no matches expected";

  DestroyTabHandle(std::move(tab));
}

}  // namespace aurelian
