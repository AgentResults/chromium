// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/handles/browser/tabstrip_handle.h"

#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_tabstrip.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "url/gurl.h"

namespace aurelian {

int OpenTab(Browser* browser, const std::string& url, bool foreground) {
  GURL gurl(url);
  if (!browser || !gurl.is_valid()) {
    return -1;
  }
  chrome::AddTabAt(browser, gurl, /*index=*/-1, foreground);
  return browser->tab_strip_model()->count() - 1;
}

bool CloseTab(Browser* browser, int index) {
  if (!browser) {
    return false;
  }
  TabStripModel* model = browser->tab_strip_model();
  if (index < 0 || index >= model->count() || model->count() <= 1) {
    return false;
  }
  model->CloseWebContentsAt(index, TabCloseTypes::CLOSE_USER_GESTURE);
  return true;
}

bool ActivateTab(Browser* browser, int index) {
  if (!browser) {
    return false;
  }
  TabStripModel* model = browser->tab_strip_model();
  if (index < 0 || index >= model->count()) {
    return false;
  }
  model->ActivateTabAt(index);
  return true;
}

int TabCount(Browser* browser) {
  return browser ? browser->tab_strip_model()->count() : 0;
}

int ActiveTabIndex(Browser* browser) {
  return browser ? browser->tab_strip_model()->active_index() : -1;
}

}  // namespace aurelian
