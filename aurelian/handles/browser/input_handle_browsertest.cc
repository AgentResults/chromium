// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C5.a — input synthesis handle
// (legion://chrome/browser/tabs/<id>/input).
//
// RED-first: these assertions are authored against the unbuilt
// AurelianInputHandle. Until the handle is mounted, ask("input") returns
// broken("not-callable") and every assertion below fails. The property
// proven is that synthesized events reach the page at the HARDWARE level
// (RenderWidgetHost::Forward*), with the DOM handler firing as the
// expected downstream consequence (plan B2).

#include "aurelian/handles/browser/tab_handle.h"

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

class AurelianInputBrowserTest : public InProcessBrowserTest {
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

  // Spin the renderer until `expr` (a JS expression that becomes truthy)
  // settles, then return it. Robust to the async browser->renderer hop.
  std::string PollString(const std::string& expr) {
    return content::EvalJs(
               GetWC(),
               "(async()=>{for(let i=0;i<200;i++){let v=(" + expr +
                   ");if(v!==null&&v!==undefined&&v!==''&&v!==false)return v;"
                   "await new Promise(r=>setTimeout(r,10));}return (" + expr +
                   ");})()")
        .ExtractString();
  }
};

// The input handle mounts at .../input and resolves to its URI.
IN_PROC_BROWSER_TEST_F(AurelianInputBrowserTest, InputHandleMounts) {
  NavigateTo("<body>input</body>");
  auto tab = CreateTabHandle(GetWC(), 9001);
  auto& handle = GetHandle(tab.get());

  auto input = handle->ask("input", V());
  ASSERT_EQ(input->state_kind(), StateKind::ResolvedValue);
  ASSERT_TRUE(input->resolved_value().is_string());
  EXPECT_NE(input->resolved_value().as_string().find(
                "legion://chrome/browser/tabs/9001/input"),
            std::string::npos)
      << "uri=" << input->resolved_value().as_string();

  DestroyTabHandle(std::move(tab));
}

// A synthesized mouse click reaches the page's click handler.
IN_PROC_BROWSER_TEST_F(AurelianInputBrowserTest,
                       SynthesizedClickReceivedByPage) {
  NavigateTo(
      "<body style='margin:0'>"
      "<div id='t' style='position:fixed;left:0;top:0;right:0;bottom:0'></div>"
      "<script>window.__clicked=false;"
      "document.addEventListener('click',e=>{window.__clicked=true;});"
      "</script>");
  auto tab = CreateTabHandle(GetWC(), 9002);
  auto& handle = GetHandle(tab.get());

  auto input = handle->ask("input", V());
  ASSERT_EQ(input->state_kind(), StateKind::ResolvedValue);
  input->tell("mouse", V::make_object({
                           {"type", V("click")},
                           {"x", V(40)},
                           {"y", V(40)},
                       }));

  EXPECT_EQ("yes", PollString("window.__clicked?'yes':''"));

  DestroyTabHandle(std::move(tab));
}

// A synthesized key event reaches the page's keydown handler with e.key.
IN_PROC_BROWSER_TEST_F(AurelianInputBrowserTest, KeyDownReceivedByPage) {
  NavigateTo(
      "<body><script>window.__key='';"
      "document.addEventListener('keydown',e=>{window.__key=e.key;});"
      "</script></body>");
  auto tab = CreateTabHandle(GetWC(), 9003);
  auto& handle = GetHandle(tab.get());

  auto input = handle->ask("input", V());
  ASSERT_EQ(input->state_kind(), StateKind::ResolvedValue);
  input->tell("key", V::make_object({
                         {"type", V("down")},
                         {"key", V("a")},
                     }));

  EXPECT_EQ("a", PollString("window.__key"));

  DestroyTabHandle(std::move(tab));
}

// Synthesized text is composed of hardware key events and enters an input.
IN_PROC_BROWSER_TEST_F(AurelianInputBrowserTest, TypeTextEntersInput) {
  NavigateTo("<body><input id='i'><script>document.getElementById('i')"
             ".focus();</script></body>");
  auto tab = CreateTabHandle(GetWC(), 9004);
  auto& handle = GetHandle(tab.get());

  auto input = handle->ask("input", V());
  ASSERT_EQ(input->state_kind(), StateKind::ResolvedValue);
  input->tell("text", V::make_object({{"text", V("hello")}}));

  EXPECT_EQ("hello",
            PollString("document.getElementById('i').value"));

  DestroyTabHandle(std::move(tab));
}

// A synthesized wheel event reaches the page at the hardware level.
IN_PROC_BROWSER_TEST_F(AurelianInputBrowserTest, WheelReceivedByPage) {
  NavigateTo(
      "<body style='height:3000px'><script>window.__dy=0;"
      "document.addEventListener('wheel',e=>{window.__dy=e.deltaY;});"
      "</script></body>");
  auto tab = CreateTabHandle(GetWC(), 9005);
  auto& handle = GetHandle(tab.get());

  auto input = handle->ask("input", V());
  ASSERT_EQ(input->state_kind(), StateKind::ResolvedValue);
  input->tell("wheel", V::make_object({
                           {"x", V(40)},
                           {"y", V(40)},
                           {"deltaY", V(120)},
                       }));

  EXPECT_EQ("yes", PollString("window.__dy!=0?'yes':''"));

  DestroyTabHandle(std::move(tab));
}

// A synthesized touch tap (gesture, touchscreen source) reaches the page.
IN_PROC_BROWSER_TEST_F(AurelianInputBrowserTest, TouchTapReceivedByPage) {
  NavigateTo(
      "<body style='margin:0'>"
      "<div id='t' style='position:fixed;left:0;top:0;right:0;bottom:0'></div>"
      "<script>window.__tap=false;"
      "document.addEventListener('click',e=>{window.__tap=true;});"
      "</script>");
  auto tab = CreateTabHandle(GetWC(), 9006);
  auto& handle = GetHandle(tab.get());

  auto input = handle->ask("input", V());
  ASSERT_EQ(input->state_kind(), StateKind::ResolvedValue);
  input->tell("touch", V::make_object({
                           {"type", V("tap")},
                           {"x", V(40)},
                           {"y", V(40)},
                       }));

  EXPECT_EQ("yes", PollString("window.__tap?'yes':''"));

  DestroyTabHandle(std::move(tab));
}

}  // namespace aurelian
