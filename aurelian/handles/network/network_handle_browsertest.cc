// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C4 regression tests — network intercept + cookies.

#include "aurelian/handles/network/network_handle.h"

#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/navigation_entry.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "net/dns/mock_host_resolver.h"
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
  }

  void TearDownOnMainThread() override {
    ClearInterceptRules();
    InProcessBrowserTest::TearDownOnMainThread();
  }

  GURL TestURL(const std::string& path) {
    return embedded_test_server()->GetURL(path);
  }

  content::BrowserContext* GetBrowserContext() {
    return browser()->profile();
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
    return nullptr;  // 404
  }
};

// --- Intercept tests ---

IN_PROC_BROWSER_TEST_F(AurelianNetworkBrowserTest,
                        DefaultPassThrough) {
  // No rules — page loads normally.
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), TestURL("/hello")));
  auto* wc = browser()->tab_strip_model()->GetActiveWebContents();
  EXPECT_FALSE(wc->GetController().GetLastCommittedEntry()->GetPageType()
               == content::PAGE_TYPE_ERROR);
}

IN_PROC_BROWSER_TEST_F(AurelianNetworkBrowserTest,
                        InterceptBlocksRequest) {
  InterceptRule rule;
  rule.url_pattern = "/blocked";
  rule.action = "block";
  int rule_id = AddInterceptRule(rule);
  EXPECT_GT(rule_id, 0);

  // Navigate to /blocked — should fail.
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), TestURL("/blocked")));
  auto* wc = browser()->tab_strip_model()->GetActiveWebContents();
  // Blocked pages result in an error page.
  EXPECT_TRUE(wc->GetController().GetLastCommittedEntry()->GetPageType()
              == content::PAGE_TYPE_ERROR);

  // Remove the rule.
  EXPECT_TRUE(RemoveInterceptRule(rule_id));
}

IN_PROC_BROWSER_TEST_F(AurelianNetworkBrowserTest,
                        InterceptRuleAddRemove) {
  InterceptRule rule;
  rule.url_pattern = "example.com";
  rule.action = "block";
  int id = AddInterceptRule(rule);
  EXPECT_EQ(GetInterceptRules().size(), 1u);

  EXPECT_TRUE(RemoveInterceptRule(id));
  EXPECT_EQ(GetInterceptRules().size(), 0u);

  EXPECT_FALSE(RemoveInterceptRule(id));  // already removed
}

// --- Cookie tests ---

IN_PROC_BROWSER_TEST_F(AurelianNetworkBrowserTest, CookieSetGetDelete) {
  std::string url = TestURL("/set-cookie").spec();

  // Set a cookie.
  auto set_result = SetCookie(GetBrowserContext(), url, "test_name", "test_val");
  EXPECT_TRUE(set_result.ok) << "set error: " << set_result.error;

  // Get it back.
  auto get_result = GetCookie(GetBrowserContext(), url, "test_name");
  EXPECT_TRUE(get_result.ok) << "get error: " << get_result.error;
  EXPECT_EQ(get_result.value, "test_val");

  // Delete it.
  auto del_result = DeleteCookie(GetBrowserContext(), url, "test_name");
  EXPECT_TRUE(del_result.ok) << "delete error: " << del_result.error;

  // Get should now fail.
  auto gone = GetCookie(GetBrowserContext(), url, "test_name");
  EXPECT_FALSE(gone.ok);
}

IN_PROC_BROWSER_TEST_F(AurelianNetworkBrowserTest,
                        CookieGetNonExistent) {
  auto result = GetCookie(GetBrowserContext(), "http://example.com/",
                          "nonexistent");
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error, "not-found");
}

}  // namespace aurelian
