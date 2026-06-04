// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C13.a browser tests — bookmarks add / list / remove.

#include "aurelian/handles/browser/bookmarks_handle.h"

#include "chrome/browser/bookmarks/bookmark_model_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "components/bookmarks/browser/bookmark_model.h"
#include "components/bookmarks/test/bookmark_test_helpers.h"
#include "content/public/test/browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace aurelian {

class AurelianBookmarksBrowserTest : public InProcessBrowserTest {
 protected:
  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    bookmarks::test::WaitForBookmarkModelToLoad(Model());
  }

  // Fetched fresh each call (no dangling member across browser teardown).
  bookmarks::BookmarkModel* Model() {
    return BookmarkModelFactory::GetForBrowserContext(browser()->profile());
  }

  bool InList(const std::vector<BookmarkInfo>& list, const std::string& url,
              std::string* title_out) {
    for (const auto& b : list) {
      if (b.url == url) {
        if (title_out) *title_out = b.title;
        return true;
      }
    }
    return false;
  }
};

IN_PROC_BROWSER_TEST_F(AurelianBookmarksBrowserTest, AddListRemove) {
  const std::string url = "https://aurelian.example.com/";

  // Add.
  ASSERT_TRUE(AddBookmark(Model(), "Aurelian", url));

  // List shows it with the right title.
  std::string title;
  ASSERT_TRUE(InList(ListBookmarks(Model()), url, &title))
      << "added bookmark missing from list";
  EXPECT_EQ(title, "Aurelian");

  // Remove.
  EXPECT_TRUE(RemoveBookmark(Model(), url));
  EXPECT_FALSE(InList(ListBookmarks(Model()), url, nullptr))
      << "bookmark still present after remove";

  // Removing a non-existent url fails.
  EXPECT_FALSE(RemoveBookmark(Model(), "https://nope.example.com/"));
}

IN_PROC_BROWSER_TEST_F(AurelianBookmarksBrowserTest, Folders) {
  const std::string url = "https://aurelian.example.com/in-folder";
  ASSERT_TRUE(AddBookmarkInFolder(Model(), "Work", "Folder Bookmark", url));

  // The bookmark is inside the folder...
  std::string title;
  ASSERT_TRUE(InList(ListFolder(Model(), "Work"), url, &title));
  EXPECT_EQ(title, "Folder Bookmark");

  // ...and NOT at the bookmark bar's top level.
  EXPECT_FALSE(InList(ListBookmarks(Model()), url, nullptr));

  // A non-existent folder lists nothing.
  EXPECT_TRUE(ListFolder(Model(), "Nope").empty());
}

}  // namespace aurelian
