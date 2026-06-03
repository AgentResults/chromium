// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/handles/browser/find_glue.h"

#include "base/memory/raw_ptr.h"
#include "base/scoped_observation.h"
#include "components/find_in_page/find_notification_details.h"
#include "components/find_in_page/find_result_observer.h"
#include "components/find_in_page/find_tab_helper.h"
#include "components/find_in_page/find_types.h"
#include "content/public/browser/web_contents.h"
#include "ui/gfx/geometry/rect.h"

namespace aurelian {
namespace {

FindResultData ToData(const find_in_page::FindNotificationDetails& r) {
  FindResultData d;
  d.ok = true;
  d.match_count = r.number_of_matches();
  d.current_match = r.active_match_ordinal();
  const gfx::Rect& rect = r.selection_rect();
  d.x = rect.x();
  d.y = rect.y();
  d.width = rect.width();
  d.height = rect.height();
  return d;
}

find_in_page::FindTabHelper* GetOrCreateHelper(content::WebContents* wc) {
  // FindTabHelper is attached lazily by Chrome (when the find bar is first
  // used); create it here if absent. No delegate is needed — the core find
  // path notifies observers directly.
  find_in_page::FindTabHelper::CreateForWebContents(wc);
  return find_in_page::FindTabHelper::FromWebContents(wc);
}

// Self-owned observer: settles `on_done` with the FINAL result for the
// request it was created for, then deletes itself.
class FindResultWaiter : public find_in_page::FindResultObserver {
 public:
  FindResultWaiter(content::WebContents* wc,
                   find_in_page::FindTabHelper* helper,
                   int request_id,
                   base::OnceCallback<void(FindResultData)> on_done)
      : wc_(wc),
        helper_(helper),
        request_id_(request_id),
        on_done_(std::move(on_done)) {
    obs_.Observe(helper);
  }

  void OnFindResultAvailable(content::WebContents* wc) override {
    if (wc != wc_) return;
    const find_in_page::FindNotificationDetails& r = helper_->find_result();
    if (r.request_id() != request_id_) return;
    if (!r.final_update()) return;  // wait for the last incremental update
    Finish(ToData(r));
  }

  void OnFindTabHelperDestroyed(find_in_page::FindTabHelper*) override {
    Finish(FindResultData{});  // ok == false
  }

 private:
  void Finish(FindResultData data) {
    obs_.Reset();
    auto cb = std::move(on_done_);
    // `this` is deleted before running the callback so a re-entrant find
    // started from within the callback cannot collide with this waiter.
    delete this;
    if (cb) std::move(cb).Run(data);
  }

  raw_ptr<content::WebContents> wc_;
  raw_ptr<find_in_page::FindTabHelper> helper_;
  int request_id_;
  base::OnceCallback<void(FindResultData)> on_done_;
  base::ScopedObservation<find_in_page::FindTabHelper,
                          find_in_page::FindResultObserver>
      obs_{this};
};

}  // namespace

void StartFindAndObserve(content::WebContents* wc,
                         const std::u16string& query,
                         bool forward,
                         bool case_sensitive,
                         base::OnceCallback<void(FindResultData)> on_done) {
  find_in_page::FindTabHelper* helper = GetOrCreateHelper(wc);
  if (!helper) {
    std::move(on_done).Run(FindResultData{});
    return;
  }
  helper->StartFinding(query, forward, case_sensitive, /*find_match=*/true);
  // Self-owned; deletes itself once the final result arrives (or the helper
  // is destroyed).
  new FindResultWaiter(wc, helper, helper->current_find_request_id(),
                       std::move(on_done));
}

void FindStep(content::WebContents* wc,
              const std::u16string& query,
              bool forward,
              bool case_sensitive) {
  if (query.empty()) return;
  find_in_page::FindTabHelper* helper = GetOrCreateHelper(wc);
  if (!helper) return;
  helper->StartFinding(query, forward, case_sensitive, /*find_match=*/true);
}

void StopFind(content::WebContents* wc) {
  find_in_page::FindTabHelper* helper =
      find_in_page::FindTabHelper::FromWebContents(wc);
  if (helper)
    helper->StopFinding(find_in_page::SelectionAction::kClear);
}

bool CurrentFindResult(content::WebContents* wc, FindResultData* out) {
  find_in_page::FindTabHelper* helper =
      find_in_page::FindTabHelper::FromWebContents(wc);
  if (!helper) return false;
  *out = ToData(helper->find_result());
  return true;
}

}  // namespace aurelian
