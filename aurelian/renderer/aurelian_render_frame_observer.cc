// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/renderer/aurelian_render_frame_observer.h"

#include <map>
#include <sstream>
#include <unordered_map>
#include <string>
#include <vector>

#include "aurelian/capability/cap_chain.h"
#include "aurelian/capability/cap_membrane.h"
#include "aurelian/capability/cap_predicate.h"
#include "aurelian/capability/cap_wire.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "content/public/renderer/render_frame.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
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

// One VeliteSink Remote (+ its producer timer) per active subscription.
// Defined before the destructor so the std::unique_ptr<RendererStream>
// vector can be destroyed (needs the complete type).
struct AurelianRenderFrameObserver::RendererStream {
  std::string name;
  mojo::Remote<aurelian::mojom::VeliteSink> sink;
  base::RepeatingTimer timer;
  int counter = 0;
};

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
  // An envelope may carry a cap (C8). The cap is verified + enforced HERE, in
  // the renderer membrane — the browser hub does not get to vouch.
  bool has_cap = false;
  std::vector<CapLink> chain;
  std::string msg;
  if (!DecodeEnvelope(envelope, &has_cap, &chain, &msg)) {
    std::string err = "{\"error\":\"cap-chain-invalid\"}";
    std::move(callback).Run(std::vector<uint8_t>(err.begin(), err.end()));
    return;
  }

  std::string verb, param;
  auto tab_pos = msg.find('\t');
  if (tab_pos != std::string::npos) {
    verb = msg.substr(0, tab_pos);
    param = msg.substr(tab_pos + 1);
  } else {
    verb = msg;
  }

  if (has_cap) {
    std::string denied = CheckCap(chain, verb, param);
    if (!denied.empty()) {
      std::move(callback).Run(
          std::vector<uint8_t>(denied.begin(), denied.end()));
      return;
    }
  }

  std::string reply_str = DispatchVerb(verb, param);
  std::vector<uint8_t> reply(reply_str.begin(), reply_str.end());
  std::move(callback).Run(reply);
}

void AurelianRenderFrameObserver::SetTrustAnchor(
    const std::vector<uint8_t>& anchor_pub) {
  if (anchor_pub.size() != trusted_anchor_.size()) {
    has_anchor_ = false;
    return;
  }
  std::copy(anchor_pub.begin(), anchor_pub.end(), trusted_anchor_.begin());
  has_anchor_ = true;
}

namespace {

// Verbs that mutate the page (a mode=read cap must deny these).
bool IsMutatingVerb(const std::string& verb) {
  return verb == "dom.node.setAttribute" || verb == "dom.node.setText" ||
         verb == "dom.node.click" || verb == "dom.node.focus" ||
         verb == "dom.node.remove" || verb == "dom.node.forget" ||
         verb == "js.eval";
}

}  // namespace

std::string AurelianRenderFrameObserver::CheckCap(
    const std::vector<CapLink>& chain,
    const std::string& verb,
    const std::string& target) {
  // The renderer membrane is one instance of the shared cap check; it differs
  // only in its verb→mutation classification.
  return EnforceCap(chain, trusted_anchor_, has_anchor_, verb,
                    IsMutatingVerb(verb), target, base::Time::Now().ToTimeT());
}

// ---------------------------------------------------------------------------
// C6 subscription streams — one VeliteSink Remote per subscription.
// ---------------------------------------------------------------------------
// Installs an idempotent MutationObserver that pushes a label per mutation
// into a main-world queue the native drain timer reads. Setting
// __aurelian_mut_init lets a subscriber confirm the observer is live before
// mutating.
constexpr char kInstallMutationObserverJs[] = R"JS(
(function(){
  if (window.__aurelian_mut_init) return true;
  window.__aurelian_mut_init = true;
  window.__aurelian_mut_queue = [];
  var mo = new MutationObserver(function(muts){
    for (var i=0;i<muts.length;i++){
      var m = muts[i];
      var t = m.target;
      var label = m.type + ':' + (t.id ? '#'+t.id : t.nodeName);
      if (m.type === 'attributes') label += ':' + m.attributeName;
      window.__aurelian_mut_queue.push(label);
    }
  });
  mo.observe(document.documentElement || document,
             {childList:true, subtree:true, attributes:true, characterData:true});
  window.__aurelian_mut_observer = mo;
  return true;
})()
)JS";

// Installs an idempotent console interceptor that wraps console.{log,info,warn,
// error,debug}, pushing a `{level,text}` JSON object per call into a main-world
// queue the native drain timer reads. Original console behaviour is preserved
// (devtools still sees the messages). __aurelian_console_init lets a subscriber
// confirm the interceptor is live before logging.
constexpr char kInstallConsoleInterceptorJs[] = R"JS(
(function(){
  if (window.__aurelian_console_init) return true;
  window.__aurelian_console_init = true;
  window.__aurelian_console_queue = [];
  var levels = ['log','info','warn','error','debug'];
  levels.forEach(function(lvl){
    var orig = console[lvl] ? console[lvl].bind(console) : null;
    console[lvl] = function(){
      try {
        var parts = [];
        for (var i=0;i<arguments.length;i++){
          var a = arguments[i];
          if (typeof a === 'string') { parts.push(a); }
          else { try { parts.push(JSON.stringify(a)); } catch(e){ parts.push(String(a)); } }
        }
        window.__aurelian_console_queue.push(
            JSON.stringify({level:lvl, text:parts.join(' ')}));
      } catch(e){}
      if (orig) orig.apply(console, arguments);
    };
  });
  return true;
})()
)JS";

// Installs an idempotent capture-phase event tap on the document for a default
// set of DOM event types, pushing a `{type,target}` JSON object per event into
// a main-world queue the native drain timer reads. Capture phase so an event is
// seen even if a handler stops propagation. __aurelian_events_init lets a
// subscriber confirm the tap is live before dispatching.
constexpr char kInstallEventTapJs[] = R"JS(
(function(){
  if (window.__aurelian_events_init) return true;
  window.__aurelian_events_init = true;
  window.__aurelian_events_queue = [];
  var types = ['click','dblclick','mousedown','mouseup','keydown','keyup',
               'input','change','submit','focus','blur','scroll'];
  types.forEach(function(ty){
    document.addEventListener(ty, function(e){
      try {
        var t = e.target;
        var label = (t && t.id) ? '#'+t.id
                  : (t && t.nodeName ? t.nodeName : 'unknown');
        window.__aurelian_events_queue.push(
            JSON.stringify({type:e.type, target:label}));
      } catch(err){}
    }, true);
  });
  return true;
})()
)JS";

void AurelianRenderFrameObserver::Subscribe(
    const std::vector<uint8_t>& envelope,
    SubscribeCallback callback) {
  std::string stream(envelope.begin(), envelope.end());

  auto rs = std::make_unique<RendererStream>();
  rs->name = stream;
  // Bind a VeliteSink Remote; its receiver goes back to the caller (browser),
  // which implements VeliteSink and receives Frame() calls.
  mojo::PendingReceiver<aurelian::mojom::VeliteSink> receiver =
      rs->sink.BindNewPipeAndPassReceiver();

  // Install the producer BEFORE replying, so a subscriber that waits for the
  // reply (or for __aurelian_mut_init / __aurelian_console_init) cannot race
  // ahead of the producer.
  if (stream == "mutations") {
    EvalString(kInstallMutationObserverJs);
  } else if (stream == "console") {
    EvalString(kInstallConsoleInterceptorJs);
  } else if (stream == "events") {
    EvalString(kInstallEventTapJs);
  }

  std::move(callback).Run(std::move(receiver));

  RendererStream* raw = rs.get();
  // Browser dropping its sink => cancel: stop + drop the stream.
  rs->sink.set_disconnect_handler(
      base::BindOnce(&AurelianRenderFrameObserver::OnStreamDisconnect,
                     weak_factory_.GetWeakPtr(), raw));

  // Start the producer timer. "test" is a periodic tick (C6.b); "mutations"
  // drains the MutationObserver queue (C6.c).
  if (stream == "test") {
    raw->timer.Start(
        FROM_HERE, base::Milliseconds(20),
        base::BindRepeating(&AurelianRenderFrameObserver::EmitTestFrame,
                            weak_factory_.GetWeakPtr(), raw));
  } else if (stream == "mutations") {
    raw->timer.Start(
        FROM_HERE, base::Milliseconds(50),
        base::BindRepeating(&AurelianRenderFrameObserver::DrainMutationFrames,
                            weak_factory_.GetWeakPtr(), raw));
  } else if (stream == "console") {
    raw->timer.Start(
        FROM_HERE, base::Milliseconds(50),
        base::BindRepeating(&AurelianRenderFrameObserver::DrainConsoleFrames,
                            weak_factory_.GetWeakPtr(), raw));
  } else if (stream == "events") {
    raw->timer.Start(
        FROM_HERE, base::Milliseconds(50),
        base::BindRepeating(&AurelianRenderFrameObserver::DrainEventFrames,
                            weak_factory_.GetWeakPtr(), raw));
  }

  streams_.push_back(std::move(rs));
}

std::string AurelianRenderFrameObserver::EvalString(const std::string& js) {
  if (!render_frame() || !render_frame()->GetWebFrame()) return std::string();
  auto* frame = render_frame()->GetWebFrame();
  auto* isolate = frame->GetAgentGroupScheduler()->Isolate();
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Value> result = frame->ExecuteScriptAndReturnValue(
      blink::WebScriptSource(blink::WebString::FromUTF8(js)));
  if (result.IsEmpty() || !result->IsString()) return std::string();
  v8::String::Utf8Value utf8(isolate, result);
  return std::string(*utf8, utf8.length());
}

void AurelianRenderFrameObserver::EmitTestFrame(RendererStream* stream) {
  std::string f = "tick-" + base::NumberToString(stream->counter++);
  stream->sink->Frame(std::vector<uint8_t>(f.begin(), f.end()));
}

void AurelianRenderFrameObserver::EmitJoinedFrames(RendererStream* stream,
                                                   const std::string& joined) {
  if (joined.empty()) return;
  size_t start = 0;
  while (start <= joined.size()) {
    size_t end = joined.find('\x01', start);
    std::string piece = (end == std::string::npos)
                            ? joined.substr(start)
                            : joined.substr(start, end - start);
    if (!piece.empty()) {
      stream->sink->Frame(std::vector<uint8_t>(piece.begin(), piece.end()));
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
}

void AurelianRenderFrameObserver::DrainMutationFrames(RendererStream* stream) {
  // Pull and clear the queued mutation labels (separated by \x01).
  EmitJoinedFrames(stream, EvalString(
      "(window.__aurelian_mut_queue?"
      "window.__aurelian_mut_queue.splice(0).join(String.fromCharCode(1)):'')"));
}

void AurelianRenderFrameObserver::DrainConsoleFrames(RendererStream* stream) {
  // Pull and clear the queued {level,text} JSON frames (separated by \x01).
  EmitJoinedFrames(stream, EvalString(
      "(window.__aurelian_console_queue?"
      "window.__aurelian_console_queue.splice(0).join(String.fromCharCode(1))"
      ":'')"));
}

void AurelianRenderFrameObserver::DrainEventFrames(RendererStream* stream) {
  // Pull and clear the queued {type,target} JSON frames (separated by \x01).
  EmitJoinedFrames(stream, EvalString(
      "(window.__aurelian_events_queue?"
      "window.__aurelian_events_queue.splice(0).join(String.fromCharCode(1))"
      ":'')"));
}

void AurelianRenderFrameObserver::OnStreamDisconnect(RendererStream* stream) {
  for (auto it = streams_.begin(); it != streams_.end(); ++it) {
    if (it->get() == stream) {
      streams_.erase(it);
      return;
    }
  }
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

  // --- DOM: focused element ---
  if (verb == "dom.focusedElement") {
    blink::WebElement focused = frame->GetDocument().FocusedElement();
    if (focused.IsNull()) return "null";
    return std::to_string(registry_->IdFor(focused));
  }

  // --- DOM: current selection text ---
  if (verb == "dom.selection") {
    if (!frame->HasSelection()) return "\"\"";
    return JsonStr(frame->SelectionAsText().Utf8());
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

  if (verb == "dom.node.getAttribute") {
    // param: "nodeId\tname"
    std::istringstream iss(param);
    std::string id_str, name;
    std::getline(iss, id_str, '\t');
    std::getline(iss, name, '\t');
    int node_id = 0;
    if (!ParseInt(id_str, &node_id)) return "{\"error\":\"bad-node-id\"}";
    auto el = registry_->NodeFor(node_id);
    if (el.IsNull()) return "{\"error\":\"gone\"}";
    blink::WebString attr_name = blink::WebString::FromUTF8(name);
    if (!el.HasAttribute(attr_name)) return "null";
    return JsonStr(el.GetAttribute(attr_name).Utf8());
  }

  if (verb == "dom.node.style") {
    // param: "nodeId\tproperty" — returns the computed CSS value.
    std::istringstream iss(param);
    std::string id_str, prop;
    std::getline(iss, id_str, '\t');
    std::getline(iss, prop, '\t');
    int node_id = 0;
    if (!ParseInt(id_str, &node_id)) return "{\"error\":\"bad-node-id\"}";
    auto el = registry_->NodeFor(node_id);
    if (el.IsNull()) return "{\"error\":\"gone\"}";
    return JsonStr(el.GetComputedValue(blink::WebString::FromUTF8(prop)).Utf8());
  }

  if (verb == "dom.node.scrollIntoView") {
    int node_id = 0;
    if (!ParseInt(param, &node_id)) return "{\"error\":\"bad-node-id\"}";
    auto el = registry_->NodeFor(node_id);
    if (el.IsNull()) return "{\"error\":\"gone\"}";
    el.ScrollIntoViewIfNeeded();
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
