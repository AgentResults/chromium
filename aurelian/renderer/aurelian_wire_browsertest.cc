// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C2/C3 regression tests — Mojo wire + DOM + JS.

#include "aurelian/public/mojom/aurelian_wire.mojom.h"

#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
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

// --- C3.8: NodeRegistry lifetime tests ---

IN_PROC_BROWSER_TEST_F(AurelianWireBrowserTest,
                        NodeRegistryPrunedOnNavigation) {
  NavigateToTestPage();
  // Query a node on page A, get its id.
  auto ids = Dispatch("dom.query", "div");
  ASSERT_EQ(ids[0], '[');
  std::string first_id = ids.substr(1, ids.find_first_of(",]") - 1);

  // Navigate to a different page (page B).
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(),
      GURL("data:text/html,<html><body><p>page B</p></body></html>")));

  // Old node-id should be "gone" — registry was pruned on navigation.
  auto text = Dispatch("dom.node.text", first_id);
  EXPECT_NE(text.find("gone"), std::string::npos)
      << "expected gone after navigation, got: " << text;
}

IN_PROC_BROWSER_TEST_F(AurelianWireBrowserTest, DetachedNodeReturnsGone) {
  NavigateToTestPage();
  auto ids = Dispatch("dom.query", "#target");
  ASSERT_NE(ids, "[]");
  std::string first_id = ids.substr(1, ids.find_first_of(",]") - 1);

  // Remove the node from the document via JS.
  Dispatch("js.eval", "document.getElementById('target').remove()");

  // Now querying the detached node should return "gone".
  auto text = Dispatch("dom.node.text", first_id);
  EXPECT_NE(text.find("gone"), std::string::npos)
      << "expected gone for detached node, got: " << text;
}

IN_PROC_BROWSER_TEST_F(AurelianWireBrowserTest,
                        RepeatedQueryDoesNotGrowUnbounded) {
  NavigateToTestPage();
  // Query the same selector 100 times.
  for (int i = 0; i < 100; i++) {
    Dispatch("dom.query", "div");
  }

  // Registry size should be bounded (1 div on the page = 1 entry).
  auto size_str = Dispatch("registry.size");
  int size = 0;
  ASSERT_TRUE(base::StringToInt(size_str, &size))
      << "registry.size=" << size_str;
  EXPECT_LE(size, 5) << "registry grew to " << size
                      << " after 100 repeated queries";
}

// A dedicated getAttribute verb reads a node's attribute value (C3 only had
// setAttribute + readback inside the self-test).
IN_PROC_BROWSER_TEST_F(AurelianWireBrowserTest, GetAttributeReadsValue) {
  NavigateToTestPage();
  auto ids = Dispatch("dom.query", "#target");
  ASSERT_NE(ids, "[]");
  std::string id = ids.substr(1, ids.find_first_of(",]") - 1);

  // Set an attribute, then read it back via the dedicated getAttribute verb.
  Dispatch("dom.node.setAttribute", id + "\tdata-x\thello");
  auto val = Dispatch("dom.node.getAttribute", id + "\tdata-x");
  EXPECT_NE(val.find("hello"), std::string::npos) << "got: " << val;

  // A missing attribute reads as null.
  auto missing = Dispatch("dom.node.getAttribute", id + "\tdata-absent");
  EXPECT_EQ(missing, "null") << "got: " << missing;
}

// dom.node.style reads a computed CSS property (the design's /dom/node style;
// C3 shipped rect but not computed style).
IN_PROC_BROWSER_TEST_F(AurelianWireBrowserTest, ComputedStyleReadsValue) {
  NavigateToTestPage();
  auto ids = Dispatch("dom.query", "#target");
  ASSERT_NE(ids, "[]");
  std::string id = ids.substr(1, ids.find_first_of(",]") - 1);

  // Apply an inline style, then read the computed value of a property.
  Dispatch("dom.node.setAttribute", id + "\tstyle\tcolor: rgb(0, 128, 0)");
  auto color = Dispatch("dom.node.style", id + "\tcolor");
  EXPECT_NE(color.find("128"), std::string::npos) << "got: " << color;
}

// dom.node.scrollIntoView scrolls a below-the-fold node into view.
IN_PROC_BROWSER_TEST_F(AurelianWireBrowserTest, ScrollIntoViewScrolls) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(),
      GURL("data:text/html,<div style='height:3000px'></div>"
           "<div id='bottom'>x</div>")));
  auto ids = Dispatch("dom.query", "#bottom");
  ASSERT_NE(ids, "[]");
  std::string id = ids.substr(1, ids.find_first_of(",]") - 1);

  // Starts at the top.
  EXPECT_EQ(Dispatch("js.eval", "window.scrollY"), "0");

  Dispatch("dom.node.scrollIntoView", id);

  // Now scrolled down (the node was below the fold).
  EXPECT_NE(Dispatch("js.eval", "window.scrollY"), "0");
}

// dom.focusedElement returns the document's focused element node-id ("null" if
// none).
IN_PROC_BROWSER_TEST_F(AurelianWireBrowserTest, FocusedElementReportsFocus) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<input id='inp'>")));
  auto ids = Dispatch("dom.query", "#inp");
  ASSERT_NE(ids, "[]");
  std::string id = ids.substr(1, ids.find_first_of(",]") - 1);

  // Focus the input, then ask which element is focused.
  Dispatch("dom.node.focus", id);
  auto focused = Dispatch("dom.focusedElement");
  EXPECT_EQ(focused, id) << "focused=" << focused;
  // And it is the input element.
  EXPECT_EQ(Dispatch("dom.node.tagName", focused), "\"INPUT\"");
}

// dom.selection returns the frame's current selected text.
IN_PROC_BROWSER_TEST_F(AurelianWireBrowserTest, SelectionReadsSelectedText) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<p>select me please</p>")));

  // No selection yet.
  EXPECT_EQ(Dispatch("dom.selection"), "\"\"");

  // Select all the document's text, then read it back.
  Dispatch("js.eval", "window.getSelection().selectAllChildren(document.body)");
  auto sel = Dispatch("dom.selection");
  EXPECT_NE(sel.find("select me please"), std::string::npos) << "sel=" << sel;
}

}  // namespace aurelian
