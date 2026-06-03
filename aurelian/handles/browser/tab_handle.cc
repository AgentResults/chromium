// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/handles/browser/tab_handle.h"

#include "aurelian/handles/browser/find_glue.h"

#include <cctype>
#include <map>
#include <optional>
#include <string>
#include <variant>

#include "aurelian/public/mojom/aurelian_wire.mojom.h"
#include "base/base64.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/ref_counted_memory.h"
#include "base/memory/weak_ptr.h"
#include "base/task/bind_post_task.h"
#include "content/public/browser/global_routing_id.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "components/input/native_web_keyboard_event.h"
#include "components/printing/browser/print_to_pdf/pdf_print_job.h"
#include "components/printing/browser/print_to_pdf/pdf_print_result.h"
#include "components/printing/browser/print_to_pdf/pdf_print_utils.h"
#include "components/printing/common/print.mojom.h"
#include "components/viz/common/frame_sinks/copy_output_result.h"
#include "components/zoom/zoom_controller.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "printing/buildflags/buildflags.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/geometry/size.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"
#include "third_party/blink/public/common/input/web_gesture_device.h"
#include "third_party/blink/public/common/input/web_gesture_event.h"
#include "third_party/blink/public/common/input/web_mouse_event.h"
#include "third_party/blink/public/common/input/web_mouse_wheel_event.h"
#include "ui/events/base_event_utils.h"
#include "ui/events/keycodes/dom/dom_code.h"
#include "ui/events/keycodes/dom/dom_key.h"
#include "ui/events/keycodes/dom/keycode_converter.h"
#include "ui/events/keycodes/keyboard_code_conversion.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/rect.h"
#include "velite/agentspaces-wire/agentspace.hpp"
#include "velite/agentspaces-wire/handle.hpp"
#include "velite/agentspaces-wire/value_handle.hpp"

namespace aurelian {
namespace {

using V = velite::agentspaces::Value;
using VH = velite::agentspaces::ValueHandle;
using Handle = velite::agentspaces::Handle;
using StateKind = velite::agentspaces::StateKind;

// ---------------------------------------------------------------------------
// Global tab registry — maps tab_id to (WebContents*, shared TabHandle).
// All access on the UI thread.
// ---------------------------------------------------------------------------
struct TabEntry {
  base::WeakPtr<content::WebContents> wc;
  std::shared_ptr<Handle> handle;
  int64_t id;
};

std::map<int64_t, TabEntry>& Registry() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  static auto* reg = new std::map<int64_t, TabEntry>();
  return *reg;
}

// ---------------------------------------------------------------------------
// NavigationHandle — drives content::NavigationController
// ---------------------------------------------------------------------------
class AurelianNavigationHandle : public Handle {
 public:
  static std::shared_ptr<AurelianNavigationHandle> make(
      base::WeakPtr<content::WebContents> wc,
      int64_t tab_id) {
    return std::shared_ptr<AurelianNavigationHandle>(
        new AurelianNavigationHandle(std::move(wc), tab_id));
  }

  StateKind state_kind() const override { return StateKind::ResolvedValue; }
  const V& resolved_value() const override { return value_; }
  std::shared_ptr<Handle> resolved_handle() const override { return nullptr; }
  std::string_view broken_reason() const override { return ""; }
  std::string sturdy_identity() const override { return uri_; }

  std::shared_ptr<Handle> ask_impl(std::string_view msg,
                                   const V& /*spec*/) override {
    DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
    auto* wc = wc_.get();
    if (!wc) return VH::make_broken("gone");
    auto& ctrl = wc->GetController();

    if (msg == "__getIdentity") return VH::make(V(uri_));
    if (msg == "canGoBack") return VH::make(V(ctrl.CanGoBack()));
    if (msg == "canGoForward") return VH::make(V(ctrl.CanGoForward()));
    if (msg == "currentIndex")
      return VH::make(V(static_cast<int64_t>(ctrl.GetCurrentEntryIndex())));
    if (msg == "entries")
      return VH::make(V(static_cast<int64_t>(ctrl.GetEntryCount())));
    return VH::make_broken("not-callable");
  }

  void tell(std::string_view msg, const V& data) override {
    DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
    auto* wc = wc_.get();
    if (!wc) return;
    auto& ctrl = wc->GetController();

    if (msg == "loadUrl") {
      if (data.is_string()) {
        content::NavigationController::LoadURLParams params(
            GURL(data.as_string()));
        params.transition_type = ui::PAGE_TRANSITION_TYPED;
        ctrl.LoadURLWithParams(params);
      } else if (data.is_object()) {
        const V* url_val = data.object_get("url");
        if (url_val && url_val->is_string()) {
          content::NavigationController::LoadURLParams params(
              GURL(url_val->as_string()));
          params.transition_type = ui::PAGE_TRANSITION_TYPED;
          ctrl.LoadURLWithParams(params);
        }
      }
    } else if (msg == "reload") {
      ctrl.Reload(content::ReloadType::NORMAL, false);
    } else if (msg == "goBack") {
      if (ctrl.CanGoBack()) ctrl.GoBack();
    } else if (msg == "goForward") {
      if (ctrl.CanGoForward()) ctrl.GoForward();
    } else if (msg == "stop") {
      wc->Stop();
    }
  }

 private:
  AurelianNavigationHandle(base::WeakPtr<content::WebContents> wc,
                           int64_t tab_id)
      : wc_(std::move(wc)),
        uri_("legion://chrome/browser/tabs/" +
             base::NumberToString(tab_id) + "/navigation"),
        value_(uri_) {}

  base::WeakPtr<content::WebContents> wc_;
  std::string uri_;
  V value_;
};

// ---------------------------------------------------------------------------
// PendingMojoHandle — starts Pending, settles to ResolvedValue or Broken
// when the Mojo reply arrives (or the pipe disconnects).
// ---------------------------------------------------------------------------
class PendingMojoHandle : public Handle {
 public:
  static std::shared_ptr<PendingMojoHandle> make() {
    return std::shared_ptr<PendingMojoHandle>(new PendingMojoHandle());
  }

  StateKind state_kind() const override { return kind_; }
  const V& resolved_value() const override { return value_; }
  std::shared_ptr<Handle> resolved_handle() const override { return nullptr; }
  std::string_view broken_reason() const override { return broken_; }
  std::string sturdy_identity() const override { return ""; }

  std::shared_ptr<Handle> ask_impl(std::string_view, const V&) override {
    return VH::make_broken("not-callable");
  }
  void tell(std::string_view, const V&) override {}

  void Resolve(const std::string& reply) {
    value_ = V(reply);
    kind_ = StateKind::ResolvedValue;
  }

  void Break(const std::string& reason) {
    broken_ = reason;
    kind_ = StateKind::Broken;
  }

 private:
  PendingMojoHandle() = default;
  StateKind kind_ = StateKind::Pending;
  V value_;
  std::string broken_;
};

// ---------------------------------------------------------------------------
// MojoDispatchState — owns the AssociatedRemote across the async gap.
// Moved into the reply/disconnect callbacks via weak pointers on the
// PendingMojoHandle so the handle settles when the reply arrives or
// the pipe drops.
// ---------------------------------------------------------------------------
struct MojoDispatchState {
  std::unique_ptr<mojo::AssociatedRemote<aurelian::mojom::AurelianWire>> wire;
  std::weak_ptr<PendingMojoHandle> pending;

  static void OnReply(std::unique_ptr<MojoDispatchState> state,
                      const std::vector<uint8_t>& reply) {
    if (auto h = state->pending.lock()) {
      h->Resolve(std::string(reply.begin(), reply.end()));
    }
    // state (and the AssociatedRemote it owns) destroyed here.
  }

  static void OnDisconnect(std::unique_ptr<MojoDispatchState> state) {
    if (auto h = state->pending.lock()) {
      h->Break("renderer-unreachable");
    }
  }
};

// ---------------------------------------------------------------------------
// FrameHandle — dispatches verbs to renderer via Mojo AurelianWire
// ---------------------------------------------------------------------------
class AurelianFrameHandle : public Handle {
 public:
  static std::shared_ptr<AurelianFrameHandle> make(
      content::RenderFrameHost* rfh,
      int64_t tab_id) {
    return std::shared_ptr<AurelianFrameHandle>(
        new AurelianFrameHandle(rfh->GetGlobalId(), tab_id,
                                rfh->GetRoutingID()));
  }

  StateKind state_kind() const override { return StateKind::ResolvedValue; }
  const V& resolved_value() const override { return value_; }
  std::shared_ptr<Handle> resolved_handle() const override { return nullptr; }
  std::string_view broken_reason() const override { return ""; }
  std::string sturdy_identity() const override { return uri_; }

  std::shared_ptr<Handle> ask_impl(std::string_view msg,
                                   const V& spec) override {
    DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
    auto* rfh = content::RenderFrameHost::FromID(rfh_id_);
    if (!rfh) return VH::make_broken("gone");

    if (msg == "__getIdentity") return VH::make(V(uri_));

    return AsyncMojoDispatch(rfh, std::string(msg), spec);
  }

  void tell(std::string_view msg, const V& data) override {
    DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
    auto* rfh = content::RenderFrameHost::FromID(rfh_id_);
    if (!rfh) return;
    AsyncMojoDispatch(rfh, std::string(msg), data);
  }

 private:
  AurelianFrameHandle(content::GlobalRenderFrameHostId id,
                      int64_t tab_id,
                      int routing_id)
      : rfh_id_(id),
        uri_("legion://chrome/browser/tabs/" +
             base::NumberToString(tab_id) + "/frames/" +
             base::NumberToString(routing_id)),
        value_(uri_) {}

  std::shared_ptr<Handle> AsyncMojoDispatch(content::RenderFrameHost* rfh,
                                            const std::string& verb,
                                            const V& spec) {
    if (!rfh->IsRenderFrameLive()) {
      return VH::make_broken("renderer-unreachable");
    }

    auto state = std::make_unique<MojoDispatchState>();
    state->wire =
        std::make_unique<mojo::AssociatedRemote<aurelian::mojom::AurelianWire>>();
    rfh->GetRemoteAssociatedInterfaces()->GetInterface(state->wire.get());
    if (!state->wire->is_bound()) {
      return VH::make_broken("renderer-unreachable");
    }

    // Build envelope: "verb" or "verb\tparam"
    std::string msg = verb;
    if (spec.is_string()) {
      msg += "\t" + spec.as_string();
    } else if (spec.is_object()) {
      for (auto& [k, v] : spec.as_object()) {
        if (v.is_string()) {
          msg += "\t" + v.as_string();
          break;
        }
      }
    }

    auto pending = PendingMojoHandle::make();
    state->pending = pending;

    // Set disconnect handler BEFORE dispatching.
    // We need a raw pointer to state for the disconnect handler since
    // BindOnce for Dispatch takes ownership. Use a weak reference.
    auto* wire_ptr = state->wire.get();

    // Move state into the Dispatch callback. The disconnect handler
    // uses a weak ref to the pending handle directly.
    std::weak_ptr<PendingMojoHandle> weak_pending = pending;
    wire_ptr->set_disconnect_handler(base::BindOnce(
        [](std::weak_ptr<PendingMojoHandle> wp) {
          if (auto h = wp.lock()) {
            h->Break("renderer-unreachable");
          }
        },
        weak_pending));

    std::vector<uint8_t> envelope(msg.begin(), msg.end());
    (*wire_ptr)->Dispatch(
        envelope,
        base::BindOnce(&MojoDispatchState::OnReply, std::move(state)));

    return pending;
  }

  content::GlobalRenderFrameHostId rfh_id_;
  std::string uri_;
  V value_;
};

// ---------------------------------------------------------------------------
// Input synthesis helpers (C5.a) — read spec fields, map keys, build events.
// ---------------------------------------------------------------------------
double NumField(const V& spec, const char* key, double dflt) {
  const V* v = spec.object_get(key);
  if (!v) return dflt;
  if (v->is_int()) return static_cast<double>(v->as_int());
  if (v->is_double()) return v->as_double();
  return dflt;
}

std::string StrField(const V& spec, const char* key, const std::string& dflt) {
  const V* v = spec.object_get(key);
  return (v && v->is_string()) ? v->as_string() : dflt;
}

int ModifiersField(const V& spec) {
  const V* v = spec.object_get("modifiers");
  return (v && v->is_int()) ? static_cast<int>(v->as_int()) : 0;
}

// Map a key name / single printable char to (DomKey, DomCode, KeyboardCode).
// The Char event carries the literal character in its text field, so text
// insertion is correct even when the keycode is only approximate.
void MapKey(const std::string& key,
            ui::DomKey* dk,
            ui::DomCode* dc,
            ui::KeyboardCode* kc) {
  if (key.size() == 1) {
    unsigned char c = static_cast<unsigned char>(key[0]);
    *dk = ui::DomKey::FromCharacter(c);
    unsigned char u = static_cast<unsigned char>(std::toupper(c));
    if (u >= 'A' && u <= 'Z') {
      *kc = static_cast<ui::KeyboardCode>(ui::VKEY_A + (u - 'A'));
    } else if (c >= '0' && c <= '9') {
      *kc = static_cast<ui::KeyboardCode>(ui::VKEY_0 + (c - '0'));
    } else if (c == ' ') {
      *kc = ui::VKEY_SPACE;
    } else {
      *kc = ui::VKEY_UNKNOWN;
    }
    *dc = ui::UsLayoutKeyboardCodeToDomCode(*kc);
    return;
  }
  if (key == "Enter") {
    *dk = ui::DomKey::ENTER;
    *kc = ui::VKEY_RETURN;
  } else if (key == "Tab") {
    *dk = ui::DomKey::TAB;
    *kc = ui::VKEY_TAB;
  } else if (key == "Backspace") {
    *dk = ui::DomKey::BACKSPACE;
    *kc = ui::VKEY_BACK;
  } else if (key == "Escape") {
    *dk = ui::DomKey::ESCAPE;
    *kc = ui::VKEY_ESCAPE;
  } else if (key == "ArrowLeft") {
    *dk = ui::DomKey::ARROW_LEFT;
    *kc = ui::VKEY_LEFT;
  } else if (key == "ArrowRight") {
    *dk = ui::DomKey::ARROW_RIGHT;
    *kc = ui::VKEY_RIGHT;
  } else if (key == "ArrowUp") {
    *dk = ui::DomKey::ARROW_UP;
    *kc = ui::VKEY_UP;
  } else if (key == "ArrowDown") {
    *dk = ui::DomKey::ARROW_DOWN;
    *kc = ui::VKEY_DOWN;
  } else {
    *dk = ui::DomKey::FromCharacter('?');
    *kc = ui::VKEY_UNKNOWN;
  }
  *dc = ui::UsLayoutKeyboardCodeToDomCode(*kc);
}

// Replicates content's BuildSimpleWebKeyEvent for production use.
void FillKeyEvent(input::NativeWebKeyboardEvent* event,
                  blink::WebInputEvent::Type type,
                  ui::DomKey dk,
                  ui::DomCode dc,
                  ui::KeyboardCode kc) {
  event->dom_key = dk;
  event->dom_code = static_cast<int>(dc);
  event->native_key_code = ui::KeycodeConverter::DomCodeToNativeKeycode(dc);
  event->windows_key_code = kc;
  event->is_system_key = false;
  event->skip_if_unhandled = true;
  if (type == blink::WebInputEvent::Type::kChar ||
      type == blink::WebInputEvent::Type::kRawKeyDown) {
    if (dk.IsCharacter()) {
      event->text[0] = dk.ToCharacter();
      event->unmodified_text[0] = dk.ToCharacter();
    } else {
      event->text[0] = kc;
      event->unmodified_text[0] = kc;
    }
  }
}

void SendKeyEvent(content::RenderWidgetHost* rwh,
                  blink::WebInputEvent::Type type,
                  ui::DomKey dk,
                  ui::DomCode dc,
                  ui::KeyboardCode kc,
                  int modifiers) {
  input::NativeWebKeyboardEvent event(type, modifiers, ui::EventTimeForNow());
  FillKeyEvent(&event, type, dk, dc, kc);
  rwh->ForwardKeyboardEvent(event);
}

// ---------------------------------------------------------------------------
// InputHandle — synthesizes hardware-level input on a WebContents'
// RenderWidgetHost (mouse/key/text/wheel/touch). All verbs are tells.
// (IME composition is renderer-interior; deferred — see tell() below.)
// ---------------------------------------------------------------------------
class AurelianInputHandle : public Handle {
 public:
  static std::shared_ptr<AurelianInputHandle> make(
      base::WeakPtr<content::WebContents> wc,
      int64_t tab_id) {
    return std::shared_ptr<AurelianInputHandle>(
        new AurelianInputHandle(std::move(wc), tab_id));
  }

  StateKind state_kind() const override { return StateKind::ResolvedValue; }
  const V& resolved_value() const override { return value_; }
  std::shared_ptr<Handle> resolved_handle() const override { return nullptr; }
  std::string_view broken_reason() const override { return ""; }
  std::string sturdy_identity() const override { return uri_; }

  std::shared_ptr<Handle> ask_impl(std::string_view msg,
                                   const V& /*spec*/) override {
    DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
    if (msg == "__getIdentity") return VH::make(V(uri_));
    return VH::make_broken("not-callable");
  }

  void tell(std::string_view msg, const V& data) override {
    DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
    auto* wc = wc_.get();
    if (!wc) return;
    auto* rfh = wc->GetPrimaryMainFrame();
    if (!rfh) return;
    auto* rwh = rfh->GetRenderWidgetHost();
    if (!rwh) return;

    if (msg == "mouse") {
      DoMouse(rwh, wc, data);
    } else if (msg == "key") {
      DoKey(rwh, data);
    } else if (msg == "text") {
      DoText(rwh, data);
    } else if (msg == "wheel") {
      DoWheel(rwh, data);
    } else if (msg == "touch") {
      DoTouch(rwh, data);
    }
    // NOTE: "ime" (composition preview + commit) is intentionally NOT
    // handled here. IME composition is a renderer-interior operation
    // (blink::WebFrameWidget / RenderWidget ImeSetComposition +
    // ImeCommitText), reached over the Mojo bridge from a renderer-side
    // input path — not the browser-process RenderWidgetHost. It is
    // deferred to that renderer handle rather than faked as text entry.
  }

 private:
  AurelianInputHandle(base::WeakPtr<content::WebContents> wc, int64_t tab_id)
      : wc_(std::move(wc)),
        uri_("legion://chrome/browser/tabs/" + base::NumberToString(tab_id) +
             "/input"),
        value_(uri_) {}

  void DoMouse(content::RenderWidgetHost* rwh,
               content::WebContents* wc,
               const V& data) {
    std::string type = StrField(data, "type", "click");
    std::string button = StrField(data, "button", "left");
    double x = NumField(data, "x", 0);
    double y = NumField(data, "y", 0);
    int modifiers = ModifiersField(data);

    auto build = [&](blink::WebInputEvent::Type t) {
      blink::WebMouseEvent e(t, modifiers, ui::EventTimeForNow());
      if (t == blink::WebInputEvent::Type::kMouseMove) {
        e.button = blink::WebMouseEvent::Button::kNoButton;
      } else if (button == "right") {
        e.button = blink::WebMouseEvent::Button::kRight;
      } else if (button == "middle") {
        e.button = blink::WebMouseEvent::Button::kMiddle;
      } else {
        e.button = blink::WebMouseEvent::Button::kLeft;
      }
      e.SetPositionInWidget(x, y);
      gfx::Rect offset = wc->GetContainerBounds();
      e.SetPositionInScreen(x + offset.x(), y + offset.y());
      e.click_count = 1;
      return e;
    };

    if (type == "move") {
      rwh->ForwardMouseEvent(build(blink::WebInputEvent::Type::kMouseMove));
    } else if (type == "down") {
      rwh->ForwardMouseEvent(build(blink::WebInputEvent::Type::kMouseDown));
    } else if (type == "up") {
      rwh->ForwardMouseEvent(build(blink::WebInputEvent::Type::kMouseUp));
    } else {  // "click" — down + up
      rwh->ForwardMouseEvent(build(blink::WebInputEvent::Type::kMouseDown));
      rwh->ForwardMouseEvent(build(blink::WebInputEvent::Type::kMouseUp));
    }
  }

  void DoKey(content::RenderWidgetHost* rwh, const V& data) {
    std::string type = StrField(data, "type", "press");
    std::string key = StrField(data, "key", "");
    if (key.empty()) return;
    int modifiers = ModifiersField(data);
    ui::DomKey dk;
    ui::DomCode dc;
    ui::KeyboardCode kc;
    MapKey(key, &dk, &dc, &kc);
    if (type == "down") {
      SendKeyEvent(rwh, blink::WebInputEvent::Type::kRawKeyDown, dk, dc, kc,
                   modifiers);
    } else if (type == "up") {
      SendKeyEvent(rwh, blink::WebInputEvent::Type::kKeyUp, dk, dc, kc,
                   modifiers);
    } else {  // "press" — down + char + up
      SendKeyEvent(rwh, blink::WebInputEvent::Type::kRawKeyDown, dk, dc, kc,
                   modifiers);
      SendKeyEvent(rwh, blink::WebInputEvent::Type::kChar, dk, dc, kc,
                   modifiers);
      SendKeyEvent(rwh, blink::WebInputEvent::Type::kKeyUp, dk, dc, kc,
                   modifiers);
    }
  }

  void DoText(content::RenderWidgetHost* rwh, const V& data) {
    std::string text = StrField(data, "text", "");
    for (char c : text) {
      std::string key(1, c);
      ui::DomKey dk;
      ui::DomCode dc;
      ui::KeyboardCode kc;
      MapKey(key, &dk, &dc, &kc);
      SendKeyEvent(rwh, blink::WebInputEvent::Type::kRawKeyDown, dk, dc, kc, 0);
      SendKeyEvent(rwh, blink::WebInputEvent::Type::kChar, dk, dc, kc, 0);
      SendKeyEvent(rwh, blink::WebInputEvent::Type::kKeyUp, dk, dc, kc, 0);
    }
  }

  void DoWheel(content::RenderWidgetHost* rwh, const V& data) {
    double x = NumField(data, "x", 0);
    double y = NumField(data, "y", 0);
    double dx = NumField(data, "deltaX", 0);
    double dy = NumField(data, "deltaY", 0);
    int modifiers = ModifiersField(data);
    blink::WebMouseWheelEvent e(blink::WebInputEvent::Type::kMouseWheel,
                                modifiers, ui::EventTimeForNow());
    e.SetPositionInWidget(x, y);
    e.delta_x = dx;
    e.delta_y = dy;
    e.phase = blink::WebMouseWheelEvent::kPhaseBegan;
    rwh->ForwardWheelEvent(e);
  }

  void DoTouch(content::RenderWidgetHost* rwh, const V& data) {
    double x = NumField(data, "x", 0);
    double y = NumField(data, "y", 0);
    // Hardware-level touchscreen gesture: TapDown then Tap, which the
    // renderer turns into pointer/click events on the page.
    blink::WebGestureEvent down(blink::WebInputEvent::Type::kGestureTapDown, 0,
                                ui::EventTimeForNow(),
                                blink::WebGestureDevice::kTouchscreen);
    down.SetPositionInWidget(gfx::PointF(x, y));
    down.data.tap_down.width = 1;
    down.data.tap_down.height = 1;
    rwh->ForwardGestureEvent(down);

    blink::WebGestureEvent tap(blink::WebInputEvent::Type::kGestureTap, 0,
                               ui::EventTimeForNow(),
                               blink::WebGestureDevice::kTouchscreen);
    tap.SetPositionInWidget(gfx::PointF(x, y));
    tap.data.tap.tap_count = 1;
    tap.data.tap.width = 1;
    tap.data.tap.height = 1;
    rwh->ForwardGestureEvent(tap);
  }

  base::WeakPtr<content::WebContents> wc_;
  std::string uri_;
  V value_;
};

// ---------------------------------------------------------------------------
// PendingValueHandle — generic async leaf: starts Pending, settles to a
// ResolvedValue or Broken when an out-of-band callback fires (C5.b+).
// ---------------------------------------------------------------------------
class PendingValueHandle : public Handle {
 public:
  static std::shared_ptr<PendingValueHandle> make() {
    return std::shared_ptr<PendingValueHandle>(new PendingValueHandle());
  }

  StateKind state_kind() const override { return kind_; }
  const V& resolved_value() const override { return value_; }
  std::shared_ptr<Handle> resolved_handle() const override { return nullptr; }
  std::string_view broken_reason() const override { return broken_; }
  std::string sturdy_identity() const override { return ""; }

  std::shared_ptr<Handle> ask_impl(std::string_view, const V&) override {
    return VH::make_broken("not-callable");
  }
  void tell(std::string_view, const V&) override {}

  void Resolve(V v) {
    value_ = std::move(v);
    kind_ = StateKind::ResolvedValue;
  }
  void Break(const std::string& reason) {
    broken_ = reason;
    kind_ = StateKind::Broken;
  }

 private:
  PendingValueHandle() = default;
  StateKind kind_ = StateKind::Pending;
  V value_;
  std::string broken_;
};

// CopyFromSurface completion: encode the bitmap as a base64 PNG and settle.
// Always invoked on the UI thread (the call is wrapped in BindPostTask).
void OnScreenshotCaptured(std::weak_ptr<PendingValueHandle> weak,
                          const content::CopyFromSurfaceResult& result) {
  auto h = weak.lock();
  if (!h) return;
  if (!result.has_value()) {
    h->Break("capture-failed");
    return;
  }
  const SkBitmap& bitmap = result->bitmap;
  if (bitmap.drawsNothing()) {
    h->Break("capture-empty");
    return;
  }
  std::optional<std::vector<uint8_t>> png =
      gfx::PNGCodec::EncodeBGRASkBitmap(bitmap, /*discard_transparency=*/false);
  if (!png) {
    h->Break("encode-failed");
    return;
  }
  h->Resolve(V(base::Base64Encode(*png)));
}

// ---------------------------------------------------------------------------
// ScreenshotHandle — compositor capture of the tab's surface (C5.b).
// ---------------------------------------------------------------------------
class AurelianScreenshotHandle : public Handle {
 public:
  static std::shared_ptr<AurelianScreenshotHandle> make(
      base::WeakPtr<content::WebContents> wc,
      int64_t tab_id) {
    return std::shared_ptr<AurelianScreenshotHandle>(
        new AurelianScreenshotHandle(std::move(wc), tab_id));
  }

  StateKind state_kind() const override { return StateKind::ResolvedValue; }
  const V& resolved_value() const override { return value_; }
  std::shared_ptr<Handle> resolved_handle() const override { return nullptr; }
  std::string_view broken_reason() const override { return ""; }
  std::string sturdy_identity() const override { return uri_; }

  std::shared_ptr<Handle> ask_impl(std::string_view msg,
                                   const V& spec) override {
    DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
    if (msg == "__getIdentity") return VH::make(V(uri_));
    if (msg == "capture") return DoCapture(spec);
    return VH::make_broken("not-callable");
  }

  void tell(std::string_view, const V&) override {}

 private:
  AurelianScreenshotHandle(base::WeakPtr<content::WebContents> wc,
                           int64_t tab_id)
      : wc_(std::move(wc)),
        uri_("legion://chrome/browser/tabs/" + base::NumberToString(tab_id) +
             "/screenshot"),
        value_(uri_) {}

  std::shared_ptr<Handle> DoCapture(const V& spec) {
    std::string format = "png";
    const V* fmt = spec.object_get("format");
    if (fmt && fmt->is_string()) format = fmt->as_string();
    if (format != "png") return VH::make_broken("unsupported-format");

    auto* wc = wc_.get();
    if (!wc) return VH::make_broken("gone");
    auto* view = wc->GetRenderWidgetHostView();
    if (!view) return VH::make_broken("gone");

    gfx::Rect src_rect;  // empty == whole surface
    const V* clip = spec.object_get("clip");
    if (clip && clip->is_object()) {
      int x = static_cast<int>(NumField(*clip, "x", 0));
      int y = static_cast<int>(NumField(*clip, "y", 0));
      int w = static_cast<int>(NumField(*clip, "width", 0));
      int h = static_cast<int>(NumField(*clip, "height", 0));
      if (w > 0 && h > 0) src_rect = gfx::Rect(x, y, w, h);
    }

    auto pending = PendingValueHandle::make();
    std::weak_ptr<PendingValueHandle> weak = pending;
    view->CopyFromSurface(
        src_rect, gfx::Size(), base::Seconds(5),
        base::BindPostTask(
            content::GetUIThreadTaskRunner({}),
            base::BindOnce(&OnScreenshotCaptured, std::move(weak))));
    return pending;
  }

  base::WeakPtr<content::WebContents> wc_;
  std::string uri_;
  V value_;
};

// ---------------------------------------------------------------------------
// PrintHandle — render the loaded page to a PDF (C5.c).
// ---------------------------------------------------------------------------
#if BUILDFLAG(ENABLE_PRINTING)
// PdfPrintJob completion: base64-encode the PDF bytes and settle. The
// PrintRenderFrame remote is carried (and kept alive) until the job ends.
void OnPrintToPdfDone(
    std::weak_ptr<PendingValueHandle> weak,
    std::unique_ptr<mojo::AssociatedRemote<printing::mojom::PrintRenderFrame>>
    /*keepalive*/,
    print_to_pdf::PdfPrintResult result,
    scoped_refptr<base::RefCountedMemory> data) {
  auto h = weak.lock();
  if (!h) return;
  if (result != print_to_pdf::PdfPrintResult::kPrintSuccess || !data) {
    h->Break("print-failed:" + print_to_pdf::PdfPrintResultToString(result));
    return;
  }
  h->Resolve(V(base::Base64Encode(base::span<const uint8_t>(*data))));
}
#endif  // BUILDFLAG(ENABLE_PRINTING)

class AurelianPrintHandle : public Handle {
 public:
  static std::shared_ptr<AurelianPrintHandle> make(
      base::WeakPtr<content::WebContents> wc,
      int64_t tab_id) {
    return std::shared_ptr<AurelianPrintHandle>(
        new AurelianPrintHandle(std::move(wc), tab_id));
  }

  StateKind state_kind() const override { return StateKind::ResolvedValue; }
  const V& resolved_value() const override { return value_; }
  std::shared_ptr<Handle> resolved_handle() const override { return nullptr; }
  std::string_view broken_reason() const override { return ""; }
  std::string sturdy_identity() const override { return uri_; }

  std::shared_ptr<Handle> ask_impl(std::string_view msg,
                                   const V& spec) override {
    DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
    if (msg == "__getIdentity") return VH::make(V(uri_));
    if (msg == "printToPdf") return DoPrint(spec);
    return VH::make_broken("not-callable");
  }

  void tell(std::string_view, const V&) override {}

 private:
  AurelianPrintHandle(base::WeakPtr<content::WebContents> wc, int64_t tab_id)
      : wc_(std::move(wc)),
        uri_("legion://chrome/browser/tabs/" + base::NumberToString(tab_id) +
             "/print"),
        value_(uri_) {}

  std::shared_ptr<Handle> DoPrint(const V& spec) {
#if BUILDFLAG(ENABLE_PRINTING)
    auto* wc = wc_.get();
    if (!wc) return VH::make_broken("gone");
    auto* rfh = wc->GetPrimaryMainFrame();
    if (!rfh || !rfh->IsRenderFrameLive()) return VH::make_broken("gone");

    auto opt_bool = [&](const char* k) -> std::optional<bool> {
      const V* v = spec.object_get(k);
      if (v && v->is_bool()) return v->as_bool();
      return std::nullopt;
    };
    auto opt_double = [&](const char* k) -> std::optional<double> {
      const V* v = spec.object_get(k);
      if (v && v->is_double()) return v->as_double();
      if (v && v->is_int()) return static_cast<double>(v->as_int());
      return std::nullopt;
    };
    auto opt_str = [&](const char* k) -> std::optional<std::string> {
      const V* v = spec.object_get(k);
      if (v && v->is_string()) return v->as_string();
      return std::nullopt;
    };

    std::string page_ranges;
    if (auto pr = opt_str("pageRanges")) page_ranges = *pr;

    std::variant<printing::mojom::PrintPagesParamsPtr, std::string> params =
        print_to_pdf::GetPrintPagesParams(
            rfh->GetLastCommittedURL(), opt_bool("landscape"),
            opt_bool("displayHeaderFooter"), opt_bool("printBackground"),
            opt_double("scale"), opt_double("paperWidth"),
            opt_double("paperHeight"), opt_double("marginTop"),
            opt_double("marginBottom"), opt_double("marginLeft"),
            opt_double("marginRight"), opt_str("headerTemplate"),
            opt_str("footerTemplate"), opt_bool("preferCssPageSize"),
            opt_bool("generateTaggedPdf"), opt_bool("generateDocumentOutline"));
    if (std::holds_alternative<std::string>(params)) {
      return VH::make_broken("bad-print-params");
    }

    auto remote = std::make_unique<
        mojo::AssociatedRemote<printing::mojom::PrintRenderFrame>>();
    rfh->GetRemoteAssociatedInterfaces()->GetInterface(remote.get());
    auto* remote_ref = remote.get();

    auto pending = PendingValueHandle::make();
    std::weak_ptr<PendingValueHandle> weak = pending;
    print_to_pdf::PdfPrintJob::StartJob(
        wc, rfh, *remote_ref, page_ranges,
        std::move(std::get<printing::mojom::PrintPagesParamsPtr>(params)),
        base::BindOnce(&OnPrintToPdfDone, std::move(weak), std::move(remote)));
    return pending;
#else
    return VH::make_broken("printing-unavailable");
#endif  // BUILDFLAG(ENABLE_PRINTING)
  }

  base::WeakPtr<content::WebContents> wc_;
  std::string uri_;
  V value_;
};

// ---------------------------------------------------------------------------
// FindHandle — find-in-page over the no-RTTI find_glue (C5.d).
// ---------------------------------------------------------------------------

// Builds the result object from the RTTI-free FindResultData.
V MakeFindResult(const FindResultData& d) {
  return V::make_object({
      {"matchCount", V(static_cast<int64_t>(d.match_count))},
      {"currentMatch", V(static_cast<int64_t>(d.current_match))},
      {"selectionRect", V::make_object({
                            {"x", V(static_cast<int64_t>(d.x))},
                            {"y", V(static_cast<int64_t>(d.y))},
                            {"width", V(static_cast<int64_t>(d.width))},
                            {"height", V(static_cast<int64_t>(d.height))},
                        })},
  });
}

class AurelianFindHandle : public Handle {
 public:
  static std::shared_ptr<AurelianFindHandle> make(
      base::WeakPtr<content::WebContents> wc,
      int64_t tab_id) {
    return std::shared_ptr<AurelianFindHandle>(
        new AurelianFindHandle(std::move(wc), tab_id));
  }

  StateKind state_kind() const override { return StateKind::ResolvedValue; }
  const V& resolved_value() const override { return value_; }
  std::shared_ptr<Handle> resolved_handle() const override { return nullptr; }
  std::string_view broken_reason() const override { return ""; }
  std::string sturdy_identity() const override { return uri_; }

  std::shared_ptr<Handle> ask_impl(std::string_view msg,
                                   const V& spec) override {
    DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
    if (msg == "__getIdentity") return VH::make(V(uri_));
    if (msg == "find") {
      const V* q = spec.object_get("query");
      if (q && q->is_string()) query_ = q->as_string();
      if (query_.empty()) return VH::make_broken("no-query");
      const V* cs = spec.object_get("caseSensitive");
      case_sensitive_ = cs && cs->is_bool() && cs->as_bool();
      return StartFind(/*forward=*/true);
    }
    if (msg == "findNext") {
      if (query_.empty()) return VH::make_broken("no-query");
      return StartFind(/*forward=*/true);
    }
    if (msg == "findPrev") {
      if (query_.empty()) return VH::make_broken("no-query");
      return StartFind(/*forward=*/false);
    }
    if (msg == "result") {
      auto* wc = wc_.get();
      if (!wc) return VH::make_broken("gone");
      FindResultData data;
      if (!CurrentFindResult(wc, &data)) return VH::make_broken("no-find");
      return VH::make(MakeFindResult(data));
    }
    return VH::make_broken("not-callable");
  }

  void tell(std::string_view msg, const V& /*data*/) override {
    DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
    auto* wc = wc_.get();
    if (!wc) return;
    if (msg == "findNext" && !query_.empty()) {
      FindStep(wc, base::UTF8ToUTF16(query_), /*forward=*/true, case_sensitive_);
    } else if (msg == "findPrev" && !query_.empty()) {
      FindStep(wc, base::UTF8ToUTF16(query_), /*forward=*/false,
               case_sensitive_);
    } else if (msg == "stop") {
      StopFind(wc);
    }
  }

 private:
  AurelianFindHandle(base::WeakPtr<content::WebContents> wc, int64_t tab_id)
      : wc_(std::move(wc)),
        uri_("legion://chrome/browser/tabs/" + base::NumberToString(tab_id) +
             "/find"),
        value_(uri_) {}

  std::shared_ptr<Handle> StartFind(bool forward) {
    auto* wc = wc_.get();
    if (!wc) return VH::make_broken("gone");

    auto pending = PendingValueHandle::make();
    std::weak_ptr<PendingValueHandle> weak = pending;
    StartFindAndObserve(
        wc, base::UTF8ToUTF16(query_), forward, case_sensitive_,
        base::BindOnce(
            [](std::weak_ptr<PendingValueHandle> weak, FindResultData d) {
              auto h = weak.lock();
              if (!h) return;
              if (!d.ok) {
                h->Break("gone");
              } else {
                h->Resolve(MakeFindResult(d));
              }
            },
            std::move(weak)));
    return pending;
  }

  base::WeakPtr<content::WebContents> wc_;
  std::string uri_;
  V value_;
  std::string query_;
  bool case_sensitive_ = false;
};

// ---------------------------------------------------------------------------
// ZoomHandle — per-tab page zoom over zoom::ZoomController (C5.e).
// ---------------------------------------------------------------------------
class AurelianZoomHandle : public Handle {
 public:
  static std::shared_ptr<AurelianZoomHandle> make(
      base::WeakPtr<content::WebContents> wc,
      int64_t tab_id) {
    return std::shared_ptr<AurelianZoomHandle>(
        new AurelianZoomHandle(std::move(wc), tab_id));
  }

  StateKind state_kind() const override { return StateKind::ResolvedValue; }
  const V& resolved_value() const override { return value_; }
  std::shared_ptr<Handle> resolved_handle() const override { return nullptr; }
  std::string_view broken_reason() const override { return ""; }
  std::string sturdy_identity() const override { return uri_; }

  std::shared_ptr<Handle> ask_impl(std::string_view msg,
                                   const V& /*spec*/) override {
    DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
    if (msg == "__getIdentity") return VH::make(V(uri_));
    zoom::ZoomController* zc = Controller();
    if (!zc) return VH::make_broken("gone");
    if (msg == "level") return VH::make(V(zc->GetZoomLevel()));
    if (msg == "percent")
      return VH::make(V(static_cast<int64_t>(zc->GetZoomPercent())));
    if (msg == "default") return VH::make(V(zc->GetDefaultZoomLevel()));
    return VH::make_broken("not-callable");
  }

  void tell(std::string_view msg, const V& data) override {
    DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
    zoom::ZoomController* zc = Controller();
    if (!zc) return;
    if (msg == "setZoom") {
      const V* level = data.object_get("level");
      if (!level) return;
      double lvl = level->is_double() ? level->as_double()
                   : level->is_int()  ? static_cast<double>(level->as_int())
                                      : 0.0;
      // Per-tab zoom: ISOLATED mode keeps this level off the shared
      // per-host zoom map, so other tabs (incl. same-host) are unaffected.
      zc->SetZoomMode(zoom::ZoomController::ZOOM_MODE_ISOLATED);
      zc->SetZoomLevel(lvl);
    } else if (msg == "reset") {
      zc->SetZoomLevel(zc->GetDefaultZoomLevel());
    }
  }

 private:
  AurelianZoomHandle(base::WeakPtr<content::WebContents> wc, int64_t tab_id)
      : wc_(std::move(wc)),
        uri_("legion://chrome/browser/tabs/" + base::NumberToString(tab_id) +
             "/zoom"),
        value_(uri_) {}

  zoom::ZoomController* Controller() {
    auto* wc = wc_.get();
    return wc ? zoom::ZoomController::FromWebContents(wc) : nullptr;
  }

  base::WeakPtr<content::WebContents> wc_;
  std::string uri_;
  V value_;
};

// ---------------------------------------------------------------------------
// TabHandle — bound to a WebContents
// ---------------------------------------------------------------------------
class AurelianTabHandle : public Handle {
 public:
  static std::shared_ptr<AurelianTabHandle> make(content::WebContents* wc,
                                                 int64_t tab_id) {
    return std::shared_ptr<AurelianTabHandle>(
        new AurelianTabHandle(wc->GetWeakPtr(), tab_id));
  }

  StateKind state_kind() const override { return StateKind::ResolvedValue; }
  const V& resolved_value() const override { return value_; }
  std::shared_ptr<Handle> resolved_handle() const override { return nullptr; }
  std::string_view broken_reason() const override { return ""; }
  std::string sturdy_identity() const override { return uri_; }

  std::shared_ptr<Handle> ask_impl(std::string_view msg,
                                   const V& spec) override {
    DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
    auto* wc = wc_.get();
    if (!wc) return VH::make_broken("gone");

    if (msg == "__getIdentity") return VH::make(V(uri_));
    if (msg == "url")
      return VH::make(V(wc->GetVisibleURL().spec()));
    if (msg == "title")
      return VH::make(V(base::UTF16ToUTF8(wc->GetTitle())));
    if (msg == "isLoading")
      return VH::make(V(wc->IsLoading()));
    if (msg == "navigation")
      return AurelianNavigationHandle::make(wc_, tab_id_);
    if (msg == "input")
      return AurelianInputHandle::make(wc_, tab_id_);
    if (msg == "screenshot")
      return AurelianScreenshotHandle::make(wc_, tab_id_);
    if (msg == "print")
      return AurelianPrintHandle::make(wc_, tab_id_);
    if (msg == "find")
      return AurelianFindHandle::make(wc_, tab_id_);
    if (msg == "zoom")
      return AurelianZoomHandle::make(wc_, tab_id_);
    if (msg == "describe") {
      return VH::make(V::make_object({
          {"uri", V(uri_)},
          {"url", V(wc->GetVisibleURL().spec())},
          {"title", V(base::UTF16ToUTF8(wc->GetTitle()))},
      }));
    }
    if (msg == "frames") {
      std::vector<V> frame_ids;
      wc->GetPrimaryMainFrame()->ForEachRenderFrameHost(
          [&](content::RenderFrameHost* rfh) {
            frame_ids.push_back(
                V(static_cast<int64_t>(rfh->GetRoutingID())));
          });
      return VH::make(V::make_array(std::move(frame_ids)));
    }
    if (msg == "frame") {
      content::RenderFrameHost* target = nullptr;
      if (spec.is_object()) {
        const V* id_val = spec.object_get("id");
        if (id_val && id_val->is_int()) {
          int want = static_cast<int>(id_val->as_int());
          wc->GetPrimaryMainFrame()->ForEachRenderFrameHost(
              [&](content::RenderFrameHost* rfh) {
                if (rfh->GetRoutingID() == want) target = rfh;
              });
        }
      }
      if (!target) target = wc->GetPrimaryMainFrame();
      if (!target) return VH::make_broken("no-frame");
      return AurelianFrameHandle::make(target, tab_id_);
    }
    return VH::make_broken("not-callable");
  }

  void tell(std::string_view /*msg*/, const V& /*data*/) override {}

  int64_t tab_id() const { return tab_id_; }

 private:
  AurelianTabHandle(base::WeakPtr<content::WebContents> wc, int64_t tab_id)
      : wc_(std::move(wc)),
        tab_id_(tab_id),
        uri_("legion://chrome/browser/tabs/" +
             base::NumberToString(tab_id)),
        value_(uri_) {}

  base::WeakPtr<content::WebContents> wc_;
  int64_t tab_id_;
  std::string uri_;
  V value_;
};

// ---------------------------------------------------------------------------
// TabsHandle — legion://chrome/browser/tabs
// ---------------------------------------------------------------------------
class AurelianTabsHandle : public Handle {
 public:
  static std::shared_ptr<AurelianTabsHandle> make() {
    return std::shared_ptr<AurelianTabsHandle>(new AurelianTabsHandle());
  }

  StateKind state_kind() const override { return StateKind::ResolvedValue; }
  const V& resolved_value() const override { return value_; }
  std::shared_ptr<Handle> resolved_handle() const override { return nullptr; }
  std::string_view broken_reason() const override { return ""; }
  std::string sturdy_identity() const override {
    return "legion://chrome/browser/tabs";
  }

  std::shared_ptr<Handle> ask_impl(std::string_view msg,
                                   const V& spec) override {
    if (msg == "__getIdentity")
      return VH::make(V("legion://chrome/browser/tabs"));

    if (msg == "list") {
      std::vector<V> items;
      for (auto& [id, entry] : Registry()) {
        auto* live_wc = entry.wc.get();
        if (!live_wc) continue;
        items.push_back(V::make_object({
            {"id", V(id)},
            {"url", V(live_wc->GetVisibleURL().spec())},
            {"title", V(base::UTF16ToUTF8(live_wc->GetTitle()))},
            {"active", V(false)},  // simplified for C1
        }));
      }
      return VH::make(V::make_array(std::move(items)));
    }

    if (msg == "tab") {
      if (spec.is_object()) {
        const V* id_val = spec.object_get("id");
        if (id_val && id_val->is_int()) {
          auto it = Registry().find(id_val->as_int());
          if (it != Registry().end()) return it->second.handle;
          return VH::make_broken("not-found");
        }
      }
      return VH::make_broken("bad-spec");
    }
    return VH::make_broken("not-callable");
  }

  void tell(std::string_view /*msg*/, const V& /*data*/) override {}

 private:
  AurelianTabsHandle() : value_("legion://chrome/browser/tabs") {}
  V value_;
};

}  // namespace

// ---------------------------------------------------------------------------
// Helpers to stash/retrieve a shared_ptr<Handle> in a void*.
// ---------------------------------------------------------------------------
void StashHandle(void*& slot, std::shared_ptr<Handle> h) {
  slot = new std::shared_ptr<Handle>(std::move(h));
}

std::shared_ptr<Handle>& GetHandle(void* slot) {
  return *static_cast<std::shared_ptr<Handle>*>(slot);
}

void ClearHandle(void*& slot) {
  if (slot) {
    delete static_cast<std::shared_ptr<Handle>*>(slot);
    slot = nullptr;
  }
}

// ---------------------------------------------------------------------------
// Public API — called from BrowserMainExtra
// ---------------------------------------------------------------------------
TabHandleImpl::~TabHandleImpl() { ClearHandle(handle_ptr); }
TabsHandleImpl::~TabsHandleImpl() { ClearHandle(handle_ptr); }

std::unique_ptr<TabHandleImpl> CreateTabHandle(content::WebContents* wc,
                                               int64_t tab_id) {
  auto impl = std::make_unique<TabHandleImpl>();
  auto handle = AurelianTabHandle::make(wc, tab_id);
  StashHandle(impl->handle_ptr, handle);
  impl->tab_id = tab_id;

  TabEntry entry;
  entry.wc = wc->GetWeakPtr();
  entry.handle = handle;
  entry.id = tab_id;
  Registry()[tab_id] = entry;

  LOG(WARNING) << "[aurelian] tab mounted: legion://chrome/browser/tabs/"
               << tab_id << " url=" << wc->GetVisibleURL().spec();
  return impl;
}

void DestroyTabHandle(std::unique_ptr<TabHandleImpl> impl) {
  if (impl) {
    Registry().erase(impl->tab_id);
    LOG(WARNING) << "[aurelian] tab unmounted: legion://chrome/browser/tabs/"
                 << impl->tab_id;
  }
}

std::unique_ptr<TabsHandleImpl> CreateTabsHandle() {
  auto impl = std::make_unique<TabsHandleImpl>();
  StashHandle(impl->handle_ptr, AurelianTabsHandle::make());
  return impl;
}

}  // namespace aurelian
