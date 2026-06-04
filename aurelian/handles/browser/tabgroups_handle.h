// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C13.c: tab groups — group / query / ungroup (browser-UI handle).

#ifndef AURELIAN_HANDLES_BROWSER_TABGROUPS_HANDLE_H_
#define AURELIAN_HANDLES_BROWSER_TABGROUPS_HANDLE_H_

#include <string>
#include <vector>

class TabStripModel;

namespace aurelian {

// Groups the tabs at `indices` into a NEW tab group; returns the group id
// string, or "" on failure (groups unsupported / empty indices). UI thread.
std::string GroupTabs(TabStripModel* model, const std::vector<int>& indices);

// Returns the group id string of the tab at `index`, "" if ungrouped. UI thread.
std::string GroupOfTab(TabStripModel* model, int index);

// Removes the tab at `index` from its group. Returns false if it wasn't grouped.
bool UngroupTab(TabStripModel* model, int index);

// Lists the id strings of all tab groups in the strip. UI thread.
std::vector<std::string> ListGroups(TabStripModel* model);

}  // namespace aurelian

#endif  // AURELIAN_HANDLES_BROWSER_TABGROUPS_HANDLE_H_
