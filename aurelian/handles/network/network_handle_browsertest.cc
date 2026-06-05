// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C4 browser tests — network intercept + cookies + storage + downloads.

#include "aurelian/handles/network/network_handle.h"

#include "base/functional/bind.h"
#include "base/run_loop.h"
#include "base/task/single_thread_task_runner.h"
#include "base/test/bind.h"
#include "base/timer/timer.h"
#include "chrome/browser/download/download_prefs.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/download_manager.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "content/public/test/download_test_observer.h"
#include "net/dns/mock_host_resolver.h"
#include "net/test/embedded_test_server/controllable_http_response.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_request.h"
#include "net/test/embedded_test_server/http_response.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace aurelian {

class AurelianNetworkBrowserTest : public InProcessBrowserTest {
 protected:
  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    host_resolver()->AddRule("*", "127.0.0.1");
    embedded_test_server()->RegisterRequestHandler(base::BindRepeating(
        &AurelianNetworkBrowserTest::HandleRequest,
        base::Unretained(this)));
    ASSERT_TRUE(embedded_test_server()->Start());
    ClearInterceptRules();
    ClearObservedRequests();
  }

  void TearDownOnMainThread() override {
    ClearInterceptRules();
    ClearObservedRequests();
    InProcessBrowserTest::TearDownOnMainThread();
  }

  GURL TestURL(const std::string& path) {
    return embedded_test_server()->GetURL(path);
  }

  content::BrowserContext* GetBrowserContext() {
    return browser()->profile();
  }

  // Spin the loop until `cond` is true or `limit` elapses.
  void PumpUntil(base::RepeatingCallback<bool()> cond, base::TimeDelta limit) {
    if (cond.Run()) return;
    base::RunLoop run_loop;
    base::RepeatingTimer timer;
    timer.Start(FROM_HERE, base::Milliseconds(10),
                base::BindRepeating(
                    [](base::RunLoop* loop, base::RepeatingCallback<bool()> c) {
                      if (c.Run()) loop->Quit();
                    },
                    &run_loop, cond));
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, run_loop.QuitClosure(), limit);
    run_loop.Run();
    timer.Stop();
  }

  void PumpFor(base::TimeDelta delay) {
    base::RunLoop run_loop;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, run_loop.QuitClosure(), delay);
    run_loop.Run();
  }

 private:
  std::unique_ptr<net::test_server::HttpResponse> HandleRequest(
      const net::test_server::HttpRequest& request) {
    auto response = std::make_unique<net::test_server::BasicHttpResponse>();
    if (request.relative_url == "/hello") {
      response->set_code(net::HTTP_OK);
      response->set_content_type("text/html");
      response->set_content("<html><body>hello from server</body></html>");
      return response;
    }
    if (request.relative_url == "/blocked") {
      response->set_code(net::HTTP_OK);
      response->set_content("should be blocked");
      return response;
    }
    if (request.relative_url == "/set-cookie") {
      response->set_code(net::HTTP_OK);
      response->set_content("ok");
      return response;
    }
    if (request.relative_url == "/mock-target") {
      response->set_code(net::HTTP_OK);
      response->set_content_type("text/plain");
      response->set_content("original server response");
      return response;
    }
    if (request.relative_url == "/fetch-page") {
      response->set_code(net::HTTP_OK);
      response->set_content_type("text/html");
      response->set_content(R"html(
        <html><body>
        <div id="result"></div>
        <script>
          fetch('/mock-target')
            .then(r => r.text())
            .then(t => { document.getElementById('result').textContent = t; })
            .catch(e => { document.getElementById('result').textContent = 'ERROR:' + e; });
        </script>
        </body></html>
      )html");
      return response;
    }
    if (request.relative_url == "/storage-page") {
      response->set_code(net::HTTP_OK);
      response->set_content_type("text/html");
      response->set_content("<html><body>storage test</body></html>");
      return response;
    }
    if (request.relative_url == "/download-file") {
      response->set_code(net::HTTP_OK);
      response->set_content_type("application/octet-stream");
      response->AddCustomHeader("Content-Disposition",
                                "attachment; filename=\"test.bin\"");
      response->set_content("download content here");
      return response;
    }
    return nullptr;  // 404
  }
};

// --- Intercept tests ---

IN_PROC_BROWSER_TEST_F(AurelianNetworkBrowserTest, DefaultPassThrough) {
  // No rules — page loads normally.
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), TestURL("/hello")));
  auto* wc = browser()->tab_strip_model()->GetActiveWebContents();
  EXPECT_FALSE(wc->GetController().GetLastCommittedEntry()->GetPageType() ==
               content::PAGE_TYPE_ERROR);
}

IN_PROC_BROWSER_TEST_F(AurelianNetworkBrowserTest, InterceptBlocksRequest) {
  InterceptRule rule;
  rule.url_pattern = "/blocked";
  rule.action = "block";
  int rule_id = AddInterceptRule(rule);
  EXPECT_GT(rule_id, 0);

  // Navigate to /blocked — should fail.
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), TestURL("/blocked")));
  auto* wc = browser()->tab_strip_model()->GetActiveWebContents();
  EXPECT_TRUE(wc->GetController().GetLastCommittedEntry()->GetPageType() ==
              content::PAGE_TYPE_ERROR);

  EXPECT_TRUE(RemoveInterceptRule(rule_id));
}

IN_PROC_BROWSER_TEST_F(AurelianNetworkBrowserTest, InterceptRuleAddRemove) {
  InterceptRule rule;
  rule.url_pattern = "example.com";
  rule.action = "block";
  int id = AddInterceptRule(rule);
  EXPECT_EQ(GetInterceptRules().size(), 1u);

  EXPECT_TRUE(RemoveInterceptRule(id));
  EXPECT_EQ(GetInterceptRules().size(), 0u);

  EXPECT_FALSE(RemoveInterceptRule(id));  // already removed
}

IN_PROC_BROWSER_TEST_F(AurelianNetworkBrowserTest,
                       InterceptReplacesResponseBody) {
  // AU-NET-BODY (#6): a "mock" rule with a body replaces the RESPONSE BODY in
  // place — the URL is unchanged and the bytes the page renders are the mock,
  // not the server's. This is real interception via the throttle's
  // URLLoaderThrottle::Delegate::InterceptResponse splice (the mechanism the
  // throttle's own TODO named), NOT a redirect to a second URL.
  InterceptRule rule;
  rule.url_pattern = "/mock-target";
  rule.action = "mock";
  rule.mock_body = "<html><body>AURELIAN MOCK BODY</body></html>";
  rule.mock_content_type = "text/html";
  int rule_id = AddInterceptRule(rule);
  EXPECT_GT(rule_id, 0);

  // /mock-target serves "original server response"; the mock must win.
  ASSERT_TRUE(
      ui_test_utils::NavigateToURL(browser(), TestURL("/mock-target")));
  auto* wc = browser()->tab_strip_model()->GetActiveWebContents();

  // The body the page rendered is the mock content...
  EXPECT_EQ(
      content::EvalJs(wc, "document.body.textContent.trim()").ExtractString(),
      "AURELIAN MOCK BODY");
  // ...the original server content is gone...
  EXPECT_EQ(
      content::EvalJs(
          wc, "document.body.textContent.indexOf('original server response')")
          .ExtractInt(),
      -1);
  // ...and the request was NOT redirected — the body was replaced in place.
  EXPECT_EQ(wc->GetLastCommittedURL(), TestURL("/mock-target"));

  EXPECT_TRUE(RemoveInterceptRule(rule_id));
}

// --- Request observation tests ---

IN_PROC_BROWSER_TEST_F(AurelianNetworkBrowserTest, ObservesRequests) {
  ClearObservedRequests();

  // Navigate to /hello — the throttle should log the request.
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), TestURL("/hello")));

  auto observed = GetObservedRequests();
  // There should be at least one request containing /hello.
  bool found = false;
  for (const auto& req : observed) {
    if (req.url.find("/hello") != std::string::npos) {
      found = true;
      EXPECT_EQ(req.method, "GET");
      break;
    }
  }
  EXPECT_TRUE(found) << "Expected to observe a request to /hello";
}

// --- Request observation STREAM tests (C6.e) ---

IN_PROC_BROWSER_TEST_F(AurelianNetworkBrowserTest, ReceivesRequestStream) {
  // Subscribe BEFORE navigating; each observed request is delivered as a frame.
  std::vector<ObservedRequest> frames;
  auto sub = SubscribeRequests(base::BindLambdaForTesting(
      [&](const ObservedRequest& r) { frames.push_back(r); }));

  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), TestURL("/hello")));

  PumpUntil(base::BindLambdaForTesting([&]() {
              for (const auto& r : frames) {
                if (r.url.find("/hello") != std::string::npos) return true;
              }
              return false;
            }),
            base::Seconds(10));

  bool found = false;
  for (const auto& r : frames) {
    if (r.url.find("/hello") != std::string::npos) {
      found = true;
      EXPECT_EQ(r.method, "GET");
      break;
    }
  }
  EXPECT_TRUE(found) << "expected a streamed request frame for /hello";
}

IN_PROC_BROWSER_TEST_F(AurelianNetworkBrowserTest, RequestStreamCancelStops) {
  std::vector<ObservedRequest> frames;
  auto sub = SubscribeRequests(base::BindLambdaForTesting(
      [&](const ObservedRequest& r) { frames.push_back(r); }));

  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), TestURL("/hello")));
  PumpUntil(base::BindLambdaForTesting([&]() { return !frames.empty(); }),
            base::Seconds(10));
  ASSERT_FALSE(frames.empty());

  // Cancel by dropping the subscription, then drain any in-flight frame.
  sub.reset();
  PumpFor(base::Milliseconds(200));
  size_t count = frames.size();

  // Navigate again — generates fresh requests, but none should be delivered.
  ASSERT_TRUE(
      ui_test_utils::NavigateToURL(browser(), TestURL("/storage-page")));
  PumpFor(base::Milliseconds(300));
  EXPECT_EQ(frames.size(), count) << "frames kept arriving after cancel";
}

// --- Download cancel (deterministic via a held-open response) ---

class AurelianDownloadCancelBrowserTest : public InProcessBrowserTest {
 protected:
  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    host_resolver()->AddRule("*", "127.0.0.1");
    slow_ = std::make_unique<net::test_server::ControllableHttpResponse>(
        embedded_test_server(), "/slow-dl");
    ASSERT_TRUE(embedded_test_server()->Start());
    ClearObservedRequests();
  }

  content::BrowserContext* GetBrowserContext() { return browser()->profile(); }

  std::unique_ptr<net::test_server::ControllableHttpResponse> slow_;
};

IN_PROC_BROWSER_TEST_F(AurelianDownloadCancelBrowserTest, CancelInProgress) {
  content::DownloadTestObserverInProgress observer(
      GetBrowserContext()->GetDownloadManager(), /*wait_count=*/1);

  StartDownload(GetBrowserContext(),
                embedded_test_server()->GetURL("/slow-dl").spec());

  // Hold the response open so the download stays in progress.
  slow_->WaitForRequest();
  slow_->Send(
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: application/octet-stream\r\n"
      "Content-Disposition: attachment; filename=\"slow.bin\"\r\n"
      "Content-Length: 1000000\r\n\r\n"
      "partial-body");
  // (No Done() — the download will not complete.)
  observer.WaitForFinished();  // wait until the download is in-progress

  // Find the in-progress download's id.
  uint32_t id = 0;
  bool found = false;
  for (const auto& dl : GetDownloads(GetBrowserContext())) {
    if (dl.url.find("/slow-dl") != std::string::npos) {
      id = dl.id;
      found = true;
    }
  }
  ASSERT_TRUE(found) << "in-progress download not observed";

  // Cancel it through the handle.
  EXPECT_TRUE(CancelDownload(GetBrowserContext(), id));

  bool cancelled = false;
  for (const auto& dl : GetDownloads(GetBrowserContext())) {
    if (dl.id == id && dl.state == "cancelled") cancelled = true;
  }
  EXPECT_TRUE(cancelled) << "download was not cancelled";

  // Cancelling an unknown id fails.
  EXPECT_FALSE(CancelDownload(GetBrowserContext(), 999999));
}

// --- Cookie tests ---

IN_PROC_BROWSER_TEST_F(AurelianNetworkBrowserTest, CookieSetGetDelete) {
  std::string url = TestURL("/set-cookie").spec();

  auto set_result =
      SetCookie(GetBrowserContext(), url, "test_name", "test_val");
  EXPECT_TRUE(set_result.ok) << "set error: " << set_result.error;

  auto get_result = GetCookie(GetBrowserContext(), url, "test_name");
  EXPECT_TRUE(get_result.ok) << "get error: " << get_result.error;
  EXPECT_EQ(get_result.value, "test_val");

  auto del_result = DeleteCookie(GetBrowserContext(), url, "test_name");
  EXPECT_TRUE(del_result.ok) << "delete error: " << del_result.error;

  auto gone = GetCookie(GetBrowserContext(), url, "test_name");
  EXPECT_FALSE(gone.ok);
}

IN_PROC_BROWSER_TEST_F(AurelianNetworkBrowserTest, CookieGetNonExistent) {
  auto result =
      GetCookie(GetBrowserContext(), "http://example.com/", "nonexistent");
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error, "not-found");
}

IN_PROC_BROWSER_TEST_F(AurelianNetworkBrowserTest, CookieGetAll) {
  const std::string url = TestURL("/hello").spec();
  ASSERT_TRUE(SetCookie(GetBrowserContext(), url, "alpha", "1").ok);
  ASSERT_TRUE(SetCookie(GetBrowserContext(), url, "beta", "2").ok);

  std::vector<CookieInfo> all = GetAllCookies(GetBrowserContext(), url);
  bool found_alpha = false, found_beta = false;
  for (const auto& c : all) {
    if (c.name == "alpha" && c.value == "1") found_alpha = true;
    if (c.name == "beta" && c.value == "2") found_beta = true;
  }
  EXPECT_TRUE(found_alpha);
  EXPECT_TRUE(found_beta);
  EXPECT_GE(all.size(), 2u);
}

IN_PROC_BROWSER_TEST_F(AurelianNetworkBrowserTest, CookieDeleteAll) {
  const std::string url = TestURL("/hello").spec();
  ASSERT_TRUE(SetCookie(GetBrowserContext(), url, "x", "1").ok);
  ASSERT_TRUE(SetCookie(GetBrowserContext(), url, "y", "2").ok);
  ASSERT_GE(GetAllCookies(GetBrowserContext(), url).size(), 2u);

  int deleted = DeleteAllCookies(GetBrowserContext(), url);
  EXPECT_GE(deleted, 2);
  EXPECT_TRUE(GetAllCookies(GetBrowserContext(), url).empty());
}

// --- Storage tests ---

IN_PROC_BROWSER_TEST_F(AurelianNetworkBrowserTest, LocalStorageReadWrite) {
  // Navigate to a real page (localStorage needs an origin).
  ASSERT_TRUE(
      ui_test_utils::NavigateToURL(browser(), TestURL("/storage-page")));
  auto* wc = browser()->tab_strip_model()->GetActiveWebContents();

  // Set a localStorage value.
  EXPECT_TRUE(
      content::ExecJs(wc, "localStorage.setItem('aurelian_key', 'aurelian_val')"));

  // Read it back.
  auto result =
      content::EvalJs(wc, "localStorage.getItem('aurelian_key')");
  EXPECT_EQ(result.ExtractString(), "aurelian_val");

  // Delete it.
  EXPECT_TRUE(content::ExecJs(wc, "localStorage.removeItem('aurelian_key')"));

  // Verify it's gone (localStorage.getItem returns null).
  EXPECT_EQ(content::EvalJs(wc, "localStorage.getItem('aurelian_key')"),
            base::Value());
}

// --- Download tests ---

IN_PROC_BROWSER_TEST_F(AurelianNetworkBrowserTest, DownloadStartAndObserve) {
  auto* dm = GetBrowserContext()->GetDownloadManager();

  // Set up observer to wait for download completion.
  content::DownloadTestObserverTerminal observer(
      dm, 1,
      content::DownloadTestObserver::ON_DANGEROUS_DOWNLOAD_ACCEPT);

  // Start the download.
  StartDownload(GetBrowserContext(), TestURL("/download-file").spec());

  // Wait for completion.
  observer.WaitForFinished();
  EXPECT_EQ(
      observer.NumDownloadsSeenInState(download::DownloadItem::COMPLETE),
      1u);

  // Verify via our GetDownloads API.
  auto downloads = GetDownloads(GetBrowserContext());
  ASSERT_FALSE(downloads.empty());

  bool found_complete = false;
  for (const auto& dl : downloads) {
    if (dl.url.find("/download-file") != std::string::npos &&
        dl.state == "complete") {
      found_complete = true;
      break;
    }
  }
  EXPECT_TRUE(found_complete)
      << "Expected a completed download of /download-file";
}

}  // namespace aurelian
