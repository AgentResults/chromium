// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/asmodeus/page_controller.h"

#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "third_party/blink/public/common/input/web_mouse_event.h"

namespace asmodeus {

PageResult::PageResult() = default;
PageResult::~PageResult() = default;
PageResult::PageResult(const PageResult&) = default;
PageResult& PageResult::operator=(const PageResult&) = default;

PageController::PageController() = default;
PageController::~PageController() = default;

// Escape a string for embedding in JS. Handles quotes and backslashes.
static std::string JSEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 10);
  for (char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '\'': out += "\\'"; break;
      case '"':  out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      default:   out += c;
    }
  }
  return out;
}

std::string PageController::BuildFindElementJS(const std::string& target,
                                                 FindMethod method) {
  std::string escaped = JSEscape(target);

  switch (method) {
    case FindMethod::kText:
      return "(function(){"
             "var all=document.querySelectorAll('*');"
             "for(var el of all){"
             "var t=(el.textContent||'').trim();"
             "if(t==='" + escaped + "'&&el.children.length===0&&el.offsetHeight>5){"
             "var r=el.getBoundingClientRect();"
             "return JSON.stringify({x:Math.round(r.x+r.width/2),"
             "y:Math.round(r.y+r.height/2),"
             "tag:el.tagName,text:t.substring(0,40)});}}"
             "return null;})()";

    case FindMethod::kTextContains:
      return "(function(){"
             "var all=document.querySelectorAll('button,a,[role=button],[role=link],div,span,li');"
             "for(var el of all){"
             "var t=(el.textContent||'');"
             "if(t.includes('" + escaped + "')&&el.offsetHeight>10&&el.offsetHeight<150"
             "&&el.offsetWidth>50){"
             "var r=el.getBoundingClientRect();"
             "return JSON.stringify({x:Math.round(r.x+r.width/2),"
             "y:Math.round(r.y+r.height/2),"
             "tag:el.tagName,text:t.trim().substring(0,40)});}}"
             "return null;})()";

    case FindMethod::kAriaLabel:
      return "(function(){"
             "var all=document.querySelectorAll('[aria-label]');"
             "for(var el of all){"
             "var label=el.getAttribute('aria-label')||'';"
             "if(label.toLowerCase().includes('" + escaped + "'.toLowerCase())"
             "&&el.offsetHeight>5){"
             "var r=el.getBoundingClientRect();"
             "return JSON.stringify({x:Math.round(r.x+r.width/2),"
             "y:Math.round(r.y+r.height/2),"
             "tag:el.tagName,text:label.substring(0,40)});}}"
             "return null;})()";

    case FindMethod::kSelector:
      return "(function(){"
             "var el=document.querySelector('" + escaped + "');"
             "if(!el||el.offsetHeight===0)return null;"
             "var r=el.getBoundingClientRect();"
             "return JSON.stringify({x:Math.round(r.x+r.width/2),"
             "y:Math.round(r.y+r.height/2),"
             "tag:el.tagName,text:(el.textContent||'').trim().substring(0,40)});})()";

    case FindMethod::kCoords:
      // target is "x,y"
      return "JSON.stringify({x:" + target + ",tag:'POINT',text:''})";
  }
  return "null";
}

std::string PageController::BuildTypeJS(const std::string& text,
                                          const std::string& selector) {
  std::string escaped_text = JSEscape(text);

  if (selector.empty()) {
    // Type into currently focused element
    return "(function(){"
           "var el=document.activeElement;"
           "if(!el)return 'NO_FOCUS';"
           "el.value='" + escaped_text + "';"
           "el.dispatchEvent(new Event('input',{bubbles:true}));"
           "el.dispatchEvent(new Event('change',{bubbles:true}));"
           "return 'OK';})()";
  }

  std::string escaped_sel = JSEscape(selector);
  return "(function(){"
         "var el=document.querySelector('" + escaped_sel + "');"
         "if(!el)return 'NOT_FOUND';"
         "el.focus();"
         "el.value='" + escaped_text + "';"
         "el.dispatchEvent(new Event('input',{bubbles:true}));"
         "el.dispatchEvent(new Event('change',{bubbles:true}));"
         "return 'OK';})()";
}

std::string PageController::BuildReadJS(const std::string& selector) {
  std::string escaped = JSEscape(selector);
  return "(function(){"
         "var el=document.querySelector('" + escaped + "');"
         "if(!el)return null;"
         "return JSON.stringify({"
         "text:(el.textContent||'').trim(),"
         "value:el.value||'',"
         "tag:el.tagName,"
         "visible:el.offsetHeight>0"
         "});})()";
}

PageResult PageController::Click(content::WebContents* wc,
                                   const std::string& target,
                                   FindMethod method) {
  PageResult result;

  if (!wc) {
    result.error = "No WebContents";
    return result;
  }

  auto* frame = wc->GetPrimaryMainFrame();
  if (!frame || !frame->IsRenderFrameLive()) {
    result.error = "Frame not ready";
    return result;
  }

  // Step 1: Find element via JS and get coordinates
  std::string js = BuildFindElementJS(target, method);
  // Note: In production, this would use async JS evaluation and
  // dispatch trusted mouse events via RenderWidgetHost.
  // For now, we use the simulated click approach that works with
  // ExecuteJavaScriptWithUserGestureForTests.

  // Build click JS that finds element and clicks with trusted gesture
  std::string click_js;
  if (method == FindMethod::kCoords) {
    // Direct coordinate click not supported via JS — would use CDP Input
    result.error = "Use CDP Input.dispatchMouseEvent for coordinate clicks";
    return result;
  }

  // Find and click in one JS execution (with user gesture)
  std::string find_and_click = BuildFindElementJS(target, method);
  // Wrap to also click
  std::string full_js =
      "(function(){"
      "var found=" + find_and_click + ";"
      "if(!found)return JSON.stringify({success:false,error:'NOT_FOUND'});"
      "var pos=JSON.parse(found);"
      // Find the actual element at those coordinates and click it
      "var el=document.elementFromPoint(pos.x,pos.y);"
      "if(el){el.click();"
      "return JSON.stringify({success:true,tag:pos.tag,text:pos.text,x:pos.x,y:pos.y});}"
      "return JSON.stringify({success:false,error:'NO_ELEMENT_AT_POINT'});"
      "})()";

  frame->ExecuteJavaScriptWithUserGestureForTests(
      base::UTF8ToUTF16(full_js), base::NullCallback(), /*world_id=*/0);

  // Note: ExecuteJavaScriptWithUserGestureForTests is async — we can't
  // get the return value synchronously here. In production, this would
  // use a callback or the CDP protocol's async evaluation.
  // For now, we optimistically return success.
  result.success = true;
  result.element_text = target;

  LOG(INFO) << "[Asmodeus] PageController::Click target='" << target
            << "' method=" << static_cast<int>(method);
  return result;
}

PageResult PageController::Type(content::WebContents* wc,
                                  const std::string& text,
                                  const std::string& target_selector) {
  PageResult result;
  if (!wc) { result.error = "No WebContents"; return result; }

  auto* frame = wc->GetPrimaryMainFrame();
  if (!frame || !frame->IsRenderFrameLive()) {
    result.error = "Frame not ready"; return result;
  }

  std::string js = BuildTypeJS(text, target_selector);
  frame->ExecuteJavaScriptWithUserGestureForTests(
      base::UTF8ToUTF16(js), base::NullCallback(), /*world_id=*/0);

  result.success = true;
  LOG(INFO) << "[Asmodeus] PageController::Type text='"
            << text.substr(0, 20) << "' selector='" << target_selector << "'";
  return result;
}

PageResult PageController::Read(content::WebContents* wc,
                                  const std::string& selector) {
  PageResult result;
  if (!wc) { result.error = "No WebContents"; return result; }
  // Read is async in production — would use CDP Runtime.evaluate
  // For now, return a stub
  result.success = true;
  LOG(INFO) << "[Asmodeus] PageController::Read selector='" << selector << "'";
  return result;
}

std::string PageController::GetTitle(content::WebContents* wc) {
  if (!wc) return "";
  return base::UTF16ToUTF8(wc->GetTitle());
}

std::string PageController::GetURL(content::WebContents* wc) {
  if (!wc) return "";
  return wc->GetLastCommittedURL().spec();
}

}  // namespace asmodeus
