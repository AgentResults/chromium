// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/federation/bridge_dispatch.h"

#include <memory>
#include <utility>

#include "aurelian/federation/completion_bridge.h"
#include "aurelian/handles/root/root_handle.h"
#include "base/functional/bind.h"
#include "content/public/browser/browser_thread.h"

namespace aurelian {

namespace {

// The production wire wait budget. The serve thread already blocks for the
// whole dispatch today (one in-flight request per wire connection, v1); the
// budget only bounds it so a never-settling command answers typed instead
// of wedging the connection forever.
constexpr base::TimeDelta kBridgeDispatchTimeout = base::Seconds(15);

// The posted UI task: starts the dispatch and RETURNS (design section 3 —
// the UI thread is free; settlement is delivered by the HS-1 session layer
// in a later turn).
void DispatchOnUi(ChromeRoot* root,
                  const std::string& path,
                  std::shared_ptr<CompletionRecord> record) {
  DispatchOutcome outcome = RootDispatch(root, path);
  if (outcome.kind == DispatchOutcome::Kind::kPending) {
    CompletionBridge::Get().RegisterPending(std::move(record),
                                            std::move(outcome.answer));
    return;
  }
  CompletionBridge::Get().Complete(record, std::move(outcome.reply));
}

}  // namespace

std::string BridgeDispatch(ChromeRoot* root, const std::string& path) {
  return BridgeDispatchWithTimeout(root, path, kBridgeDispatchTimeout);
}

std::string BridgeDispatchWithTimeout(ChromeRoot* root,
                                      const std::string& path,
                                      base::TimeDelta timeout) {
  if (content::BrowserThread::CurrentlyOn(content::BrowserThread::UI)) {
    DispatchOutcome outcome = RootDispatch(root, path);
    if (outcome.kind == DispatchOutcome::Kind::kCompleted) {
      return outcome.reply;
    }
    // A UI-thread caller cannot block on settlement without nesting a UI
    // loop — the deleted shape. Typed refusal, never a hang.
    return "broken:dispatch-would-block-ui";
  }
  // SERVE THREAD: record created + registered BEFORE posting (round-6 — the
  // one wait below is Stop()-coverable by construction).
  std::shared_ptr<CompletionRecord> record =
      CompletionBridge::Get().CreateRecord();
  content::GetUIThreadTaskRunner({})->PostTask(
      FROM_HERE,
      base::BindOnce(&DispatchOnUi, base::Unretained(root), path, record));
  return CompletionBridge::Get().Wait(record, timeout);
}

}  // namespace aurelian
