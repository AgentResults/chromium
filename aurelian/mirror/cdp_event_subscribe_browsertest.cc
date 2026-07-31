// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// ACM-4 browser tests (AURELIAN-GENERIC-CONTROL-TDD-PLAN Phase A): CDP
// events feed the substrate `subscribe` surface, IN-PROCESS (wire
// eventing is out of scope v1, design section 8; re-pathed round-4 per
// review N1 — the happy path lives on the per-target sub-mirror, NOT the
// browser mirror). A CDP event subscription IS a handle subscription
// (design section 3): subscribe({sink}) on an event node answers the
// substrate SubscriptionHandle, the sink receives every matching event
// as a `legion-notify` tell carrying the descriptor-typed payload, and
// tell("cancel") detaches. The session-context rule covers `subscribe`
// exactly like `invoke` (design section 2 round-4): a page-scoped event
// at the browser mirror — and a derived-browser-only event at a page
// sub-mirror — is the typed refusal naming the correct path; an event
// subscription is NEVER accepted-but-silently-event-less.
//
// RED: event nodes answer broken:unknown-message to subscribe — no event
// fan-in exists (cdp_session drops id-less protocol messages on the
// floor), and neither refusal direction is typed.

#include <memory>
#include <string>
#include <vector>

#include "aurelian/catalog/cdp_catalog.h"
#include "aurelian/handles/root/root_handle.h"
#include "aurelian/mirror/cdp_mirror.h"
#include "base/run_loop.h"
#include "base/task/single_thread_task_runner.h"
#include "base/test/run_until.h"
#include "base/values.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"
#include "velite/agentspaces-wire/handle.hpp"
#include "velite/agentspaces-wire/value_handle.hpp"

namespace aurelian {

namespace {

using velite::agentspaces::Handle;
using velite::agentspaces::StateKind;
using velite::agentspaces::Value;
using velite::agentspaces::ValueHandle;

// The substrate sink: records every `legion-notify` frame it is told.
class RecordingSink : public Handle {
 public:
  StateKind state_kind() const override { return StateKind::ResolvedValue; }
  const Value& resolved_value() const override { return identity_; }
  std::shared_ptr<Handle> resolved_handle() const override { return nullptr; }
  std::string_view broken_reason() const override { return ""; }
  std::string sturdy_identity() const override { return "test://sink"; }

  std::shared_ptr<Handle> ask_impl(std::string_view,
                                   const Value&) override {
    return ValueHandle::make_broken("unknown-message");
  }

  void tell(std::string_view msg, const Value& frame) override {
    if (msg == "legion-notify") {
      frames_.push_back(frame);
    }
  }

  const std::vector<Value>& frames() const { return frames_; }

 private:
  std::vector<Value> frames_;
  Value identity_{std::string("test://sink")};
};

std::shared_ptr<Handle> Walk(const std::shared_ptr<Handle>& from,
                             std::initializer_list<const char*> segments) {
  std::shared_ptr<Handle> cur = from;
  for (const char* s : segments) {
    cur = cur->ask(s, Value());
    if (!cur || cur->state_kind() == StateKind::Broken) {
      return cur;
    }
  }
  return cur;
}

Value SinkSpec(const std::shared_ptr<Handle>& sink) {
  return Value::make_object({{"sink", Value(sink)}});
}

// CDP's own enable contract: events flow only once the domain's enable
// round-trips (a real CDP client awaits the enable reply). The mirror's
// subscribe auto-enables asynchronously, so a subscriber that needs
// enabled-before-action ordering sequences an invoke on the SAME target —
// session messages are processed in order, so a settled invoke proves the
// earlier auto-enable was processed.
[[nodiscard]] bool AwaitSubscriptionEffective(
    const std::shared_ptr<Handle>& sub_mirror) {
  std::shared_ptr<Handle> probe =
      Walk(sub_mirror, {"Runtime", "evaluate"})
          ->ask("invoke",
                Value::make_object({{"expression", Value("1")}}));
  return base::test::RunUntil(
      [&]() { return probe->state_kind() != StateKind::Pending; });
}

}  // namespace

class AurelianCdpEventSubscribeBrowserTest : public InProcessBrowserTest {
 protected:
  content::WebContents* GetWC() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }

  std::string ActivePageTargetId() {
    return content::DevToolsAgentHost::GetOrCreateFor(GetWC())->GetId();
  }

  void PumpFor(base::TimeDelta delay) {
    base::RunLoop run_loop;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, run_loop.QuitClosure(), delay);
    run_loop.Run();
  }
};

// subscribe({sink}) on targets/<id>/cdp/Page/frameNavigated; navigate
// THAT tab; the event arrives as a substrate subscription frame whose
// payload is the descriptor-typed one (frameNavigated declares `frame`,
// and the arrived frame carries it, url included).
IN_PROC_BROWSER_TEST_F(AurelianCdpEventSubscribeBrowserTest,
                       SubscribeDeliversDescriptorTypedEvent) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>e0</title>acm4-sub-tab")));
  std::string id = ActivePageTargetId();

  std::shared_ptr<Handle> mirror = CreateCdpMirrorForTarget(id, "page");
  std::shared_ptr<Handle> event_node =
      Walk(mirror, {"Page", "frameNavigated"});
  ASSERT_NE(event_node, nullptr);
  ASSERT_NE(event_node->state_kind(), StateKind::Broken)
      << event_node->broken_reason();

  auto sink = std::make_shared<RecordingSink>();
  std::shared_ptr<Handle> sub =
      event_node->ask("legion-subscribe", SinkSpec(sink));
  ASSERT_NE(sub, nullptr);
  ASSERT_NE(sub->state_kind(), StateKind::Broken)
      << "no event fan-in: " << sub->broken_reason();
  ASSERT_TRUE(AwaitSubscriptionEffective(mirror));

  // Drive a real navigation on THAT tab.
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(),
      GURL("data:text/html,<title>e1</title>acm4-navigated")));

  ASSERT_TRUE(base::test::RunUntil([&]() { return !sink->frames().empty(); }))
      << "the subscribed event never arrived";

  // The descriptor's own shape: frameNavigated declares a `frame`
  // parameter — the arrived payload carries it, with THAT tab's url.
  const base::DictValue* descriptor_event =
      CdpCatalog::Get().FindEvent("Page", "frameNavigated");
  ASSERT_NE(descriptor_event, nullptr);
  bool declares_frame = false;
  for (const base::Value& p : *descriptor_event->FindList("parameters")) {
    if (const std::string* n = p.GetDict().FindString("name");
        n && *n == "frame") {
      declares_frame = true;
    }
  }
  ASSERT_TRUE(declares_frame);

  bool saw_navigated = false;
  for (const Value& frame : sink->frames()) {
    ASSERT_TRUE(frame.is_object());
    const Value* event = frame.object_get("event");
    ASSERT_NE(event, nullptr);
    EXPECT_EQ(event->as_string(), "Page.frameNavigated");
    const Value* params = frame.object_get("params");
    ASSERT_NE(params, nullptr);
    const Value* page_frame = params->object_get("frame");
    ASSERT_NE(page_frame, nullptr)
        << "payload does not carry the descriptor-declared `frame`";
    const Value* url = page_frame->object_get("url");
    if (url && url->is_string() &&
        url->as_string().find("acm4-navigated") != std::string::npos) {
      saw_navigated = true;
    }
  }
  EXPECT_TRUE(saw_navigated)
      << "no frameNavigated frame carried THAT tab's navigated url";

  // The subscription lifecycle is truthful: delivery promoted it.
  std::shared_ptr<Handle> state = sub->ask("getState", Value());
  ASSERT_NE(state, nullptr);
  EXPECT_EQ(state->resolved_value().as_string(), "active");
}

// tell("cancel") detaches: no frame is delivered after the holder
// cancels (the unsubscribe half of the plan's RED).
IN_PROC_BROWSER_TEST_F(AurelianCdpEventSubscribeBrowserTest,
                       UnsubscribeDetaches) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>u0</title>acm4-unsub-tab")));
  std::string id = ActivePageTargetId();

  std::shared_ptr<Handle> sub_mirror = CreateCdpMirrorForTarget(id, "page");
  std::shared_ptr<Handle> event_node =
      Walk(sub_mirror, {"Page", "frameNavigated"});
  ASSERT_NE(event_node->state_kind(), StateKind::Broken)
      << event_node->broken_reason();

  auto sink = std::make_shared<RecordingSink>();
  std::shared_ptr<Handle> sub =
      event_node->ask("legion-subscribe", SinkSpec(sink));
  ASSERT_NE(sub->state_kind(), StateKind::Broken)
      << "no event fan-in: " << sub->broken_reason();
  ASSERT_TRUE(AwaitSubscriptionEffective(sub_mirror));

  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>u1</title>acm4-first-nav")));
  ASSERT_TRUE(base::test::RunUntil([&]() { return !sink->frames().empty(); }));

  sub->tell("cancel", Value());
  const size_t count_at_cancel = sink->frames().size();

  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>u2</title>acm4-second-nav")));
  PumpFor(base::Milliseconds(300));
  EXPECT_EQ(sink->frames().size(), count_at_cancel)
      << "frames kept arriving after cancel";
}

// The subscription half of the session-context rule (design section 2,
// round-4), BOTH directions, rows never hand-named: a page-scoped event
// at the browser mirror refuses naming the target path; a derived
// browser-only domain's event at a page sub-mirror refuses naming the
// browser path. Never accepted-but-silently-event-less.
IN_PROC_BROWSER_TEST_F(AurelianCdpEventSubscribeBrowserTest,
                       SubscribeSessionContextRefusalBothDirections) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>r0</title>acm4-refusal-tab")));
  auto sink = std::make_shared<RecordingSink>();

  // Page-scoped event at the BROWSER mirror.
  std::shared_ptr<Handle> browser_event =
      Walk(CreateCdpMirror(), {"Page", "frameNavigated"});
  ASSERT_NE(browser_event->state_kind(), StateKind::Broken)
      << browser_event->broken_reason();
  std::shared_ptr<Handle> refused =
      browser_event->ask("legion-subscribe", SinkSpec(sink));
  ASSERT_EQ(refused->state_kind(), StateKind::Broken)
      << "accepted-but-silently-event-less at the browser mirror";
  std::string reason(refused->broken_reason());
  EXPECT_EQ(reason.rfind("session-context", 0), 0u) << reason;
  EXPECT_NE(reason.find("targets/<id>/cdp/Page/frameNavigated"),
            std::string::npos)
      << reason;

  // A derived browser-only domain's event at a PAGE sub-mirror
  // (membership from the ACM-S2 derivation ONLY; vacuous if the set is
  // empty or event-less — recorded, the browser-mirror half above
  // carries the refusal RED then).
  const CdpCatalog& cat = CdpCatalog::Get();
  std::string id = ActivePageTargetId();
  std::shared_ptr<Handle> sub_mirror = CreateCdpMirrorForTarget(id, "page");
  bool exercised = false;
  for (const std::string& domain : cat.BrowserOnlyDomains()) {
    std::vector<std::string> events = cat.EventsOf(domain);
    if (events.empty()) {
      continue;
    }
    std::shared_ptr<Handle> node =
        Walk(sub_mirror, {domain.c_str(), events.front().c_str()});
    ASSERT_NE(node->state_kind(), StateKind::Broken)
        << node->broken_reason();
    std::shared_ptr<Handle> r = node->ask("legion-subscribe", SinkSpec(sink));
    ASSERT_EQ(r->state_kind(), StateKind::Broken)
        << domain << "." << events.front()
        << " accepted-but-silently-event-less at a page sub-mirror";
    std::string page_reason(r->broken_reason());
    EXPECT_EQ(page_reason.rfind("session-context", 0), 0u) << page_reason;
    EXPECT_NE(page_reason.find("cdp/" + domain + "/" + events.front()),
              std::string::npos)
        << page_reason;
    exercised = true;
  }
  if (!exercised) {
    SUCCEED() << "derived browser-only set carries no events (recorded)";
  }
}

}  // namespace aurelian
