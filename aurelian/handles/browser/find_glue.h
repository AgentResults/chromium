// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AURELIAN_HANDLES_BROWSER_FIND_GLUE_H_
#define AURELIAN_HANDLES_BROWSER_FIND_GLUE_H_

#include <string>

#include "base/functional/callback.h"

namespace content {
class WebContents;
}

namespace aurelian {

// Plain (RTTI-free) view of a find-in-page result. Lives in this header so
// the RTTI-compiled handle code never includes find_in_page headers — its
// FindResultObserver derives from base::CheckedObserver, whose typeinfo is
// not emitted in Chromium's no-RTTI build and would otherwise leave an
// undefined typeinfo symbol when subclassed in an RTTI translation unit.
struct FindResultData {
  bool ok = false;  // false => the tab/helper went away
  int match_count = 0;
  int current_match = 0;  // 1-based ordinal of the active match
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

// Starts a find on the tab's find_in_page::FindTabHelper (creating it if
// Chrome has not yet attached it) and invokes `on_done` exactly once with
// the FINAL result for that request. UI-thread only.
void StartFindAndObserve(content::WebContents* wc,
                         const std::u16string& query,
                         bool forward,
                         bool case_sensitive,
                         base::OnceCallback<void(FindResultData)> on_done);

// Fire-and-forget stepping / stop (the FindTabHelper remembers the query).
void FindStep(content::WebContents* wc,
              const std::u16string& query,
              bool forward,
              bool case_sensitive);
void StopFind(content::WebContents* wc);

// Reads the helper's last cached result synchronously. Returns false if the
// helper does not exist.
bool CurrentFindResult(content::WebContents* wc, FindResultData* out);

}  // namespace aurelian

#endif  // AURELIAN_HANDLES_BROWSER_FIND_GLUE_H_
