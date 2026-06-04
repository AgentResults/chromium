// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/handles/browser/tabs_overview.h"

#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "content/public/browser/web_contents.h"
#include "url/gurl.h"

namespace aurelian {

TabsOverview GetTabsOverview() {
  TabsOverview out;
  // Anchor on the last-active browser (direct BrowserList iteration is private
  // now); the overview is scoped to its profile, which is the active window's.
  Browser* last = chrome::FindLastActive();
  if (!last) {
    return out;
  }
  for (Browser* browser :
       chrome::FindAllBrowsersWithProfile(last->profile())) {
    out.open_count += browser->tab_strip_model()->count();
  }
  if (content::WebContents* wc =
          last->tab_strip_model()->GetActiveWebContents()) {
    out.active_url = wc->GetLastCommittedURL().spec();
  }
  return out;
}

}  // namespace aurelian
