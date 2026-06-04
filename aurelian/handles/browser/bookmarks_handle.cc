// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/handles/browser/bookmarks_handle.h"

#include "base/location.h"
#include "base/memory/raw_ptr.h"
#include "base/strings/utf_string_conversions.h"
#include "components/bookmarks/browser/bookmark_model.h"
#include "components/bookmarks/browser/bookmark_node.h"
#include "components/bookmarks/common/bookmark_metrics.h"
#include "url/gurl.h"

namespace aurelian {

bool AddBookmark(bookmarks::BookmarkModel* model,
                 const std::string& title,
                 const std::string& url) {
  GURL gurl(url);
  if (!model || !model->loaded() || !gurl.is_valid()) {
    return false;
  }
  const bookmarks::BookmarkNode* bar = model->bookmark_bar_node();
  model->AddURL(bar, bar->children().size(), base::UTF8ToUTF16(title), gurl);
  return true;
}

std::vector<BookmarkInfo> ListBookmarks(bookmarks::BookmarkModel* model) {
  std::vector<BookmarkInfo> result;
  if (!model || !model->loaded()) {
    return result;
  }
  for (const auto& child : model->bookmark_bar_node()->children()) {
    if (child->is_url()) {
      result.push_back(
          {base::UTF16ToUTF8(child->GetTitle()), child->url().spec()});
    }
  }
  return result;
}

bool RemoveBookmark(bookmarks::BookmarkModel* model, const std::string& url) {
  GURL gurl(url);
  if (!model || !model->loaded() || !gurl.is_valid()) {
    return false;
  }
  std::vector<raw_ptr<const bookmarks::BookmarkNode, VectorExperimental>>
      nodes = model->GetNodesByURL(gurl);
  if (nodes.empty()) {
    return false;
  }
  model->Remove(nodes[0], bookmarks::metrics::BookmarkEditSource::kUser,
                FROM_HERE);
  return true;
}

}  // namespace aurelian
