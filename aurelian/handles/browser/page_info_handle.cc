// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/handles/browser/page_info_handle.h"

#include "content/public/browser/web_contents.h"

namespace aurelian {

PageContentInfo GetPageContentInfo(content::WebContents* wc) {
  PageContentInfo info;
  if (!wc) {
    return info;
  }
  info.mime_type = wc->GetContentsMimeType();
  info.encoding = wc->GetEncoding();
  return info;
}

}  // namespace aurelian
