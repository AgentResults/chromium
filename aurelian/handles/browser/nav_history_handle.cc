// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/handles/browser/nav_history_handle.h"

#include "base/strings/utf_string_conversions.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/web_contents.h"

namespace aurelian {

std::vector<HistoryEntry> GetNavigationHistory(content::WebContents* wc) {
  std::vector<HistoryEntry> result;
  if (!wc) {
    return result;
  }
  content::NavigationController& controller = wc->GetController();
  for (int i = 0; i < controller.GetEntryCount(); ++i) {
    content::NavigationEntry* entry = controller.GetEntryAtIndex(i);
    if (entry) {
      result.push_back({entry->GetVirtualURL().spec(),
                        base::UTF16ToUTF8(entry->GetTitle())});
    }
  }
  return result;
}

int CurrentHistoryIndex(content::WebContents* wc) {
  return wc ? wc->GetController().GetLastCommittedEntryIndex() : -1;
}

}  // namespace aurelian
