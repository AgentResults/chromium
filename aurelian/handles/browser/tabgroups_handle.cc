// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/handles/browser/tabgroups_handle.h"

#include <optional>

#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/tab_groups/tab_group_id.h"

namespace aurelian {

std::string GroupTabs(TabStripModel* model, const std::vector<int>& indices) {
  if (!model || !model->SupportsTabGroups() || indices.empty()) {
    return std::string();
  }
  return model->AddToNewGroup(indices).ToString();
}

std::string GroupOfTab(TabStripModel* model, int index) {
  if (!model) {
    return std::string();
  }
  std::optional<tab_groups::TabGroupId> group =
      model->GetTabGroupForTab(index);
  return group ? group->ToString() : std::string();
}

bool UngroupTab(TabStripModel* model, int index) {
  if (!model || !model->GetTabGroupForTab(index)) {
    return false;
  }
  model->RemoveFromGroup({index});
  return true;
}

}  // namespace aurelian
