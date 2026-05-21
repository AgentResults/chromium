// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C3.6 regression tests — production FrameHandle Mojo dispatch.

#include "aurelian/handles/browser/tab_handle.h"
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
#include "velite/agentspaces-wire/handle.hpp"
#include "velite/agentspaces-wire/value_handle.hpp"

namespace aurelian {

using V = velite::agentspaces::Value;
using StateKind = velite::agentspaces::StateKind;

class AurelianFrameHandleBrowserTest : public InProcessBrowserTest {
 protected:
  void NavigateToTestPage() {
    ASSERT_TRUE(ui_test_utils::NavigateToURL(
        browser(),
        GURL("data:text/html,<html><head><title>C36Test</title></head>"
             "<body><div id='target'>hello c36</div></body></html>")));
  }

  content::WebContents* GetWC() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }

  // Get the TabHandle's ask result via the void* indirection.
  std::shared_ptr<velite::agentspaces::Handle>& GetHandle(
      TabHandleImpl* impl) {
    return *static_cast<std::shared_ptr<velite::agentspaces::Handle>*>(
        impl->handle_ptr);
  }
};

IN_PROC_BROWSER_TEST_F(AurelianFrameHandleBrowserTest,
                        FramesReturnsNonEmpty) {
  NavigateToTestPage();
  auto tab = CreateTabHandle(GetWC(), 7001);
  auto& handle = GetHandle(tab.get());

  auto frames = handle->ask("frames", V());
  ASSERT_EQ(frames->state_kind(), StateKind::ResolvedValue);
  ASSERT_TRUE(frames->resolved_value().is_array());
  EXPECT_GT(frames->resolved_value().as_array().size(), 0u);

  DestroyTabHandle(std::move(tab));
}

IN_PROC_BROWSER_TEST_F(AurelianFrameHandleBrowserTest,
                        FrameAskDomReachesRenderer) {
  NavigateToTestPage();
  auto tab = CreateTabHandle(GetWC(), 7002);
  auto& handle = GetHandle(tab.get());

  // Get the primary frame (no id = default).
  auto frame = handle->ask("frame", V());
  ASSERT_NE(frame, nullptr);
  ASSERT_EQ(frame->state_kind(), StateKind::ResolvedValue);

  // Ask dom.query through the frame → round-trips Mojo to renderer.
  auto dom_result = frame->ask("dom.query", V("div"));
  ASSERT_EQ(dom_result->state_kind(), StateKind::ResolvedValue);
  ASSERT_TRUE(dom_result->resolved_value().is_string());
  std::string reply = dom_result->resolved_value().as_string();
  EXPECT_EQ(reply[0], '[') << "reply=" << reply;
  EXPECT_NE(reply, "[]") << "expected at least one div";

  DestroyTabHandle(std::move(tab));
}

IN_PROC_BROWSER_TEST_F(AurelianFrameHandleBrowserTest,
                        JsEvalThroughFrameHandle) {
  NavigateToTestPage();
  auto tab = CreateTabHandle(GetWC(), 7003);
  auto& handle = GetHandle(tab.get());

  auto frame = handle->ask("frame", V());
  ASSERT_EQ(frame->state_kind(), StateKind::ResolvedValue);

  auto result = frame->ask("js.eval", V("1+1"));
  ASSERT_EQ(result->state_kind(), StateKind::ResolvedValue);
  EXPECT_EQ(result->resolved_value().as_string(), "2");

  DestroyTabHandle(std::move(tab));
}

IN_PROC_BROWSER_TEST_F(AurelianFrameHandleBrowserTest,
                        DomNodeTextThroughFrameHandle) {
  NavigateToTestPage();
  auto tab = CreateTabHandle(GetWC(), 7004);
  auto& handle = GetHandle(tab.get());

  auto frame = handle->ask("frame", V());
  auto ids_result = frame->ask("dom.query", V("#target"));
  ASSERT_TRUE(ids_result->resolved_value().is_string());
  std::string ids = ids_result->resolved_value().as_string();
  std::string first_id = ids.substr(1, ids.find_first_of(",]") - 1);

  auto text = frame->ask("dom.node.text", V(first_id));
  ASSERT_TRUE(text->resolved_value().is_string());
  EXPECT_NE(text->resolved_value().as_string().find("hello c36"),
            std::string::npos)
      << "text=" << text->resolved_value().as_string();

  DestroyTabHandle(std::move(tab));
}

IN_PROC_BROWSER_TEST_F(AurelianFrameHandleBrowserTest,
                        FrameIdentityHasCorrectUri) {
  NavigateToTestPage();
  auto tab = CreateTabHandle(GetWC(), 7005);
  auto& handle = GetHandle(tab.get());

  auto frame = handle->ask("frame", V());
  auto ident = frame->ask("__getIdentity", V());
  ASSERT_TRUE(ident->resolved_value().is_string());
  EXPECT_NE(ident->resolved_value().as_string().find(
                "legion://chrome/browser/tabs/7005/frames/"),
            std::string::npos)
      << "identity=" << ident->resolved_value().as_string();

  DestroyTabHandle(std::move(tab));
}

}  // namespace aurelian
