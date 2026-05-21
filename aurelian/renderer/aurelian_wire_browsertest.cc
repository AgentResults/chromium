// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C2/C3 regression tests — Mojo wire + DOM + JS.

#include "aurelian/public/mojom/aurelian_wire.mojom.h"

#include "base/run_loop.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"
#include "url/gurl.h"

namespace aurelian {

class AurelianWireBrowserTest : public InProcessBrowserTest {
 protected:
  void NavigateToTestPage() {
    ASSERT_TRUE(ui_test_utils::NavigateToURL(
        browser(),
        GURL("data:text/html,<html><head><title>C3Test</title></head>"
             "<body><div id='target'>hello aurelian</div></body></html>")));
  }

  content::RenderFrameHost* GetMainFrame() {
    return browser()
        ->tab_strip_model()
        ->GetActiveWebContents()
        ->GetPrimaryMainFrame();
  }

  std::string Dispatch(const std::string& verb,
                       const std::string& param = "") {
    auto* rfh = GetMainFrame();
    mojo::AssociatedRemote<aurelian::mojom::AurelianWire> wire;
    rfh->GetRemoteAssociatedInterfaces()->GetInterface(&wire);
    if (!wire.is_bound()) return "<not-bound>";

    std::string msg = param.empty() ? verb : verb + "\t" + param;
    std::vector<uint8_t> envelope(msg.begin(), msg.end());

    std::string reply_str;
    base::RunLoop run_loop;
    wire->Dispatch(envelope,
                   base::BindOnce(
                       [](base::RunLoop* loop, std::string* out,
                          const std::vector<uint8_t>& reply) {
                         *out = std::string(reply.begin(), reply.end());
                         loop->Quit();
                       },
                       &run_loop, &reply_str));
    run_loop.Run();
    return reply_str;
  }
};

// --- C2: Mojo binding + round-trip ---

IN_PROC_BROWSER_TEST_F(AurelianWireBrowserTest, WireBound) {
  NavigateToTestPage();
  auto* rfh = GetMainFrame();
  mojo::AssociatedRemote<aurelian::mojom::AurelianWire> wire;
  rfh->GetRemoteAssociatedInterfaces()->GetInterface(&wire);
  EXPECT_TRUE(wire.is_bound());
}

IN_PROC_BROWSER_TEST_F(AurelianWireBrowserTest, DispatchRoundTrip) {
  NavigateToTestPage();
  auto reply = Dispatch("describe");
  EXPECT_NE(reply.find("renderer"), std::string::npos) << "reply=" << reply;
}

// --- C3: DOM + JS ---

IN_PROC_BROWSER_TEST_F(AurelianWireBrowserTest, DomQuery) {
  NavigateToTestPage();
  auto reply = Dispatch("dom.query", "div");
  EXPECT_EQ(reply[0], '[');
  EXPECT_NE(reply, "[]");
}

IN_PROC_BROWSER_TEST_F(AurelianWireBrowserTest, NodeTagName) {
  NavigateToTestPage();
  auto ids = Dispatch("dom.query", "div");
  std::string first_id = ids.substr(1, ids.find_first_of(",]") - 1);
  auto tagName = Dispatch("dom.node.tagName", first_id);
  EXPECT_NE(tagName.find("DIV"), std::string::npos) << "tagName=" << tagName;
}

IN_PROC_BROWSER_TEST_F(AurelianWireBrowserTest, NodeText) {
  NavigateToTestPage();
  auto ids = Dispatch("dom.query", "#target");
  std::string first_id = ids.substr(1, ids.find_first_of(",]") - 1);
  auto text = Dispatch("dom.node.text", first_id);
  EXPECT_NE(text.find("hello aurelian"), std::string::npos) << "text=" << text;
}

IN_PROC_BROWSER_TEST_F(AurelianWireBrowserTest, JsEvalNumber) {
  NavigateToTestPage();
  EXPECT_EQ(Dispatch("js.eval", "1+1"), "2");
}

IN_PROC_BROWSER_TEST_F(AurelianWireBrowserTest, JsEvalTitle) {
  NavigateToTestPage();
  auto reply = Dispatch("js.eval", "document.title");
  EXPECT_NE(reply.find("C3Test"), std::string::npos) << "title=" << reply;
}

IN_PROC_BROWSER_TEST_F(AurelianWireBrowserTest, NodeForget) {
  NavigateToTestPage();
  auto ids = Dispatch("dom.query", "div");
  std::string first_id = ids.substr(1, ids.find_first_of(",]") - 1);
  Dispatch("dom.node.forget", first_id);
  auto text = Dispatch("dom.node.text", first_id);
  EXPECT_NE(text.find("gone"), std::string::npos) << "text=" << text;
}

IN_PROC_BROWSER_TEST_F(AurelianWireBrowserTest, SelfTestAllPass) {
  NavigateToTestPage();
  auto reply = Dispatch("c3.selftest");
  EXPECT_NE(reply.find("PASS"), std::string::npos) << "reply=" << reply;
  EXPECT_EQ(reply.find("FAIL"), std::string::npos) << "reply=" << reply;
}

}  // namespace aurelian
