// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/renderer/aurelian_render_frame_observer.h"

#include <map>
#include <sstream>
#include <unordered_map>
#include <string>
#include <vector>

#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "content/public/renderer/render_frame.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_registry.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_element.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_node.h"
#include "third_party/blink/public/platform/scheduler/web_agent_group_scheduler.h"
#include "third_party/blink/public/web/web_script_source.h"
#include "v8/include/v8.h"

namespace aurelian {

// Node registry — maps integer IDs to WebElements.
// Fixed in C3.8: prune-on-navigation, IsConnected check, O(1) IdFor via
// reverse index keyed on blink DomNodeId.
struct AurelianRenderFrameObserver::NodeRegistryImpl {
  int IdFor(const blink::WebElement& element) {
    int dom_id = element.GetDomNodeId();
    auto rit = reverse.find(dom_id);
    if (rit != reverse.end()) {
      // Verify the entry is still valid.
      auto it = nodes.find(rit->second);
      if (it != nodes.end() && it->second == element) {
        return rit->second;
      }
      // Stale reverse entry — clean up.
      reverse.erase(rit);
    }
    int id = next_id++;
    nodes[id] = element;
    reverse[dom_id] = id;
    return id;
  }

  // Returns the element if still in the registry AND connected to the
  // document. Returns null WebElement if gone or detached.
  blink::WebElement NodeFor(int id) {
    auto it = nodes.find(id);
    if (it == nodes.end()) return blink::WebElement();
    if (it->second.IsNull() || !it->second.IsConnected()) {
      // Detached or collected — prune and report gone.
      int dom_id = it->second.IsNull() ? 0 : it->second.GetDomNodeId();
      reverse.erase(dom_id);
      nodes.erase(it);
      return blink::WebElement();
    }
    return it->second;
  }

  void Remove(int id) {
    auto it = nodes.find(id);
    if (it != nodes.end()) {
      if (!it->second.IsNull()) {
        reverse.erase(it->second.GetDomNodeId());
      }
      nodes.erase(it);
    }
  }

  void Clear() {
    nodes.clear();
    reverse.clear();
    next_id = 1;
  }

  size_t Size() const { return nodes.size(); }

  int next_id = 1;
  std::map<int, blink::WebElement> nodes;
  std::unordered_map<int, int> reverse;  // DomNodeId -> registry id
};

namespace {

std::string JsonStr(const std::string& s) {
  std::string result;
  base::JSONWriter::Write(base::Value(s), &result);
  return result;
}

bool ParseInt(const std::string& s, int* out) {
  return base::StringToInt(s, out);
}

}  // namespace

AurelianRenderFrameObserver::AurelianRenderFrameObserver(
    content::RenderFrame* frame)
    : content::RenderFrameObserver(frame),
      registry_(std::make_unique<NodeRegistryImpl>()) {
  frame->GetAssociatedInterfaceRegistry()
      ->AddInterface<aurelian::mojom::AurelianWire>(base::BindRepeating(
          &AurelianRenderFrameObserver::BindAurelianWire,
          base::Unretained(this)));
}

AurelianRenderFrameObserver::~AurelianRenderFrameObserver() = default;

void AurelianRenderFrameObserver::OnDestruct() {
  delete this;
}

void AurelianRenderFrameObserver::DidCommitProvisionalLoad(
    ui::PageTransition /*transition*/) {
  // New document — clear the node registry. Old node-ids become "gone".
  registry_->Clear();
}

void AurelianRenderFrameObserver::BindAurelianWire(
    mojo::PendingAssociatedReceiver<aurelian::mojom::AurelianWire> receiver) {
  receiver_.reset();
  receiver_.Bind(std::move(receiver));
}

void AurelianRenderFrameObserver::Dispatch(
    const std::vector<uint8_t>& envelope,
    DispatchCallback callback) {
  std::string msg(envelope.begin(), envelope.end());

  std::string verb, param;
  auto tab_pos = msg.find('\t');
  if (tab_pos != std::string::npos) {
    verb = msg.substr(0, tab_pos);
    param = msg.substr(tab_pos + 1);
  } else {
    verb = msg;
  }

  std::string reply_str = DispatchVerb(verb, param);
  std::vector<uint8_t> reply(reply_str.begin(), reply_str.end());
  std::move(callback).Run(reply);
}

std::string AurelianRenderFrameObserver::DispatchVerb(
    const std::string& verb,
    const std::string& param) {
  if (!render_frame() || !render_frame()->GetWebFrame())
    return "{\"error\":\"frame-gone\"}";

  auto* frame = render_frame()->GetWebFrame();

  // --- Identity ---
  if (verb == "describe" || verb == "__getIdentity")
    return "{\"origin\":\"renderer\",\"type\":\"frame\"}";

  // --- DOM: query ---
  if (verb == "dom.query") {
    auto doc = frame->GetDocument();
    auto elements = doc.QuerySelectorAll(blink::WebString::FromUTF8(param));
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < elements.size(); ++i) {
      if (i > 0) oss << ",";
      oss << registry_->IdFor(elements[i]);
    }
    oss << "]";
    return oss.str();
  }

  // --- DOM: getElementById ---
  if (verb == "dom.getElementById") {
    auto doc = frame->GetDocument();
    auto el = doc.GetElementById(blink::WebString::FromUTF8(param));
    if (el.IsNull()) return "{\"error\":\"not-found\"}";
    return std::to_string(registry_->IdFor(el));
  }

  // --- DOM: node reads ---
  if (verb == "dom.node.tagName" || verb == "dom.node.text" ||
      verb == "dom.node.html" || verb == "dom.node.rect") {
    int node_id = 0;
    if (!ParseInt(param, &node_id)) return "{\"error\":\"bad-node-id\"}";
    auto el = registry_->NodeFor(node_id);
    if (el.IsNull()) return "{\"error\":\"gone\"}";

    if (verb == "dom.node.tagName") return JsonStr(el.TagName().Utf8());
    if (verb == "dom.node.text") return JsonStr(el.TextContent().Utf8());
    if (verb == "dom.node.html") return JsonStr(el.InnerHTML().Utf8());
    if (verb == "dom.node.rect") {
      auto r = el.BoundsInWidget();
      return "{\"x\":" + std::to_string(r.x()) +
             ",\"y\":" + std::to_string(r.y()) +
             ",\"w\":" + std::to_string(r.width()) +
             ",\"h\":" + std::to_string(r.height()) + "}";
    }
  }

  // --- DOM: node mutations ---
  if (verb == "dom.node.setAttribute") {
    // param: "nodeId\tname\tvalue"
    std::istringstream iss(param);
    std::string id_str, name, value;
    std::getline(iss, id_str, '\t');
    std::getline(iss, name, '\t');
    std::getline(iss, value, '\t');
    int node_id = 0;
    if (!ParseInt(id_str, &node_id)) return "{\"error\":\"bad-node-id\"}";
    auto el = registry_->NodeFor(node_id);
    if (el.IsNull()) return "{\"error\":\"gone\"}";
    el.SetAttribute(blink::WebString::FromUTF8(name),
                    blink::WebString::FromUTF8(value));
    return "{\"ok\":true}";
  }

  if (verb == "dom.node.click") {
    int node_id = 0;
    if (!ParseInt(param, &node_id)) return "{\"error\":\"bad-node-id\"}";
    auto el = registry_->NodeFor(node_id);
    if (el.IsNull()) return "{\"error\":\"gone\"}";
    el.Click();
    return "{\"ok\":true}";
  }

  if (verb == "dom.node.focus") {
    int node_id = 0;
    if (!ParseInt(param, &node_id)) return "{\"error\":\"bad-node-id\"}";
    auto el = registry_->NodeFor(node_id);
    if (el.IsNull()) return "{\"error\":\"gone\"}";
    el.Focus();
    return "{\"ok\":true}";
  }

  // --- JS: eval ---
  if (verb == "js.eval") {
    auto* isolate = frame->GetAgentGroupScheduler()->Isolate();
    v8::HandleScope handle_scope(isolate);
    v8::Local<v8::Value> result = frame->ExecuteScriptAndReturnValue(
        blink::WebScriptSource(blink::WebString::FromUTF8(param)));
    if (result.IsEmpty() || result->IsUndefined() || result->IsNull())
      return "null";
    if (result->IsBoolean())
      return result->BooleanValue(isolate) ? "true" : "false";
    if (result->IsNumber()) {
      double d =
          result->NumberValue(isolate->GetCurrentContext()).FromMaybe(0.0);
      if (d == static_cast<int64_t>(d) && d >= -1e15 && d <= 1e15)
        return std::to_string(static_cast<int64_t>(d));
      return std::to_string(d);
    }
    if (result->IsString()) {
      v8::String::Utf8Value utf8(isolate, result);
      return JsonStr(std::string(*utf8, utf8.length()));
    }
    v8::Local<v8::String> json_str;
    if (v8::JSON::Stringify(isolate->GetCurrentContext(), result)
            .ToLocal(&json_str)) {
      v8::String::Utf8Value utf8(isolate, json_str);
      return std::string(*utf8, utf8.length());
    }
    return "{\"error\":\"unstringifiable\"}";
  }

  // --- DOM: forget node (for GC test) ---
  if (verb == "dom.node.forget") {
    int node_id = 0;
    if (!ParseInt(param, &node_id)) return "{\"error\":\"bad-node-id\"}";
    registry_->Remove(node_id);
    return "{\"ok\":true}";
  }

  // --- C3 self-test: run all DOM/JS checks in-renderer ---
  if (verb == "c3.selftest") {
    std::ostringstream out;
    auto pass = [&](const char* name) {
      out << "PASS: " << name << "\n";
    };
    auto fail = [&](const char* name, const std::string& detail) {
      out << "FAIL: " << name << " — " << detail << "\n";
    };

    auto doc = frame->GetDocument();

    // 1. dom.query("div")
    auto divs = doc.QuerySelectorAll(blink::WebString::FromUTF8("div"));
    if (divs.size() > 0) {
      pass("dom.query('div') returns non-empty");
      int div_id = registry_->IdFor(divs[0]);

      // 2. tagName
      auto el = registry_->NodeFor(div_id);
      if (!el.IsNull() && el.TagName().Utf8() == "DIV") {
        pass("dom.node.tagName = DIV");
      } else {
        fail("dom.node.tagName", el.IsNull() ? "null" : el.TagName().Utf8());
      }

      // 3. text
      std::string text = el.TextContent().Utf8();
      if (text.find("hello aurelian") != std::string::npos) {
        pass("dom.node.text contains 'hello aurelian'");
      } else {
        fail("dom.node.text", text);
      }

      // 4. setAttribute + read back
      el.SetAttribute(blink::WebString::FromUTF8("data-test"),
                      blink::WebString::FromUTF8("c3ok"));
      auto attr = el.GetAttribute(blink::WebString::FromUTF8("data-test"));
      if (attr.Utf8() == "c3ok") {
        pass("dom.node.setAttribute + readback");
      } else {
        fail("dom.node.setAttribute", attr.Utf8());
      }

      // 5. getElementById
      auto target = doc.GetElementById(blink::WebString::FromUTF8("target"));
      if (!target.IsNull()) {
        pass("dom.getElementById('target')");
      } else {
        fail("dom.getElementById('target')", "null");
      }

      // 6. node.forget (GC simulation) — forget the node, then try to access
      registry_->Remove(div_id);
      auto forgotten = registry_->NodeFor(div_id);
      if (forgotten.IsNull()) {
        pass("dom.node.forget -> gone");
      } else {
        fail("dom.node.forget", "still exists");
      }
    } else {
      fail("dom.query('div')", "empty");
    }

    // 7. js.eval("1+1")
    {
      auto* isolate = frame->GetAgentGroupScheduler()->Isolate();
      v8::HandleScope handle_scope(isolate);
      auto result = frame->ExecuteScriptAndReturnValue(
          blink::WebScriptSource(blink::WebString::FromUTF8("1+1")));
      if (!result.IsEmpty() && result->IsNumber()) {
        double d = result->NumberValue(isolate->GetCurrentContext())
                       .FromMaybe(0.0);
        if (d == 2.0) {
          pass("js.eval('1+1') = 2");
        } else {
          fail("js.eval('1+1')", std::to_string(d));
        }
      } else {
        fail("js.eval('1+1')", "not a number");
      }
    }

    // 8. js.eval("document.title")
    {
      auto* isolate = frame->GetAgentGroupScheduler()->Isolate();
      v8::HandleScope handle_scope(isolate);
      auto result = frame->ExecuteScriptAndReturnValue(
          blink::WebScriptSource(
              blink::WebString::FromUTF8("document.title")));
      if (!result.IsEmpty() && result->IsString()) {
        v8::String::Utf8Value utf8(isolate, result);
        std::string title(*utf8, utf8.length());
        if (title == "C3Test") {
          pass("js.eval('document.title') = C3Test");
        } else {
          fail("js.eval('document.title')", title);
        }
      } else {
        fail("js.eval('document.title')", "not a string");
      }
    }

    return out.str();
  }

  // --- Registry introspection (for tests) ---
  if (verb == "registry.size") {
    return std::to_string(registry_->Size());
  }

  return "{\"error\":\"not-callable\",\"verb\":" + JsonStr(verb) + "}";
}

void AurelianRenderFrameObserver::EnsureNodeBridge(
    int node_id,
    const blink::WebElement& el) {
  // Placeholder for future use.
}

}  // namespace aurelian
