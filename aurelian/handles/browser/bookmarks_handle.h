// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C13.a: bookmarks — add / list / remove (browser-UI handle).

#ifndef AURELIAN_HANDLES_BROWSER_BOOKMARKS_HANDLE_H_
#define AURELIAN_HANDLES_BROWSER_BOOKMARKS_HANDLE_H_

#include <string>
#include <vector>

namespace bookmarks {
class BookmarkModel;
}

namespace aurelian {

struct BookmarkInfo {
  std::string title;
  std::string url;
};

// Adds a bookmark to the bookmark bar. Returns false if `model` is unusable or
// `url` is invalid. UI thread.
bool AddBookmark(bookmarks::BookmarkModel* model,
                 const std::string& title,
                 const std::string& url);

// Lists the bookmark-bar bookmarks (url nodes), top level. UI thread.
std::vector<BookmarkInfo> ListBookmarks(bookmarks::BookmarkModel* model);

// Removes the first bookmark matching `url`. Returns false if none match. UI
// thread.
bool RemoveBookmark(bookmarks::BookmarkModel* model, const std::string& url);

}  // namespace aurelian

#endif  // AURELIAN_HANDLES_BROWSER_BOOKMARKS_HANDLE_H_
