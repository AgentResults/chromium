// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/handles/browser/accessibility_handle.h"

#include <map>

#include "base/json/json_writer.h"
#include "base/run_loop.h"
#include "base/values.h"
#include "content/public/browser/web_contents.h"
#include "ui/accessibility/ax_enum_util.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/accessibility/ax_mode.h"
#include "ui/accessibility/ax_node_data.h"
#include "ui/accessibility/ax_tree_update.h"

namespace aurelian {

std::vector<AxNode> SnapshotAxTree(content::WebContents* wc) {
  std::vector<AxNode> result;
  if (!wc) {
    return result;
  }

  base::RunLoop run_loop;
  wc->RequestAXTreeSnapshot(
      base::BindOnce(
          [](std::vector<AxNode>* out, base::RunLoop* loop,
             ui::AXTreeUpdate& update) {
            for (const ui::AXNodeData& node : update.nodes) {
              AxNode n;
              n.role = ui::ToString(node.role);
              n.name = node.GetStringAttribute(
                  ax::mojom::StringAttribute::kName);
              out->push_back(std::move(n));
            }
            loop->Quit();
          },
          &result, &run_loop),
      ui::kAXModeComplete, /*max_nodes=*/0,
      /*timeout=*/base::Seconds(0),
      content::WebContents::AXTreeSnapshotPolicy::kAll);
  run_loop.Run();
  return result;
}

namespace {

std::string JsonQuote(const std::string& s) {
  std::string out;
  base::JSONWriter::Write(base::Value(s), &out);
  return out;
}

// Recursively renders the node `id` and its descendants as nested JSON.
std::string RenderNode(
    const std::map<int32_t, const ui::AXNodeData*>& by_id,
    int32_t id) {
  auto it = by_id.find(id);
  if (it == by_id.end()) {
    return "null";
  }
  const ui::AXNodeData* node = it->second;
  std::string json = "{\"role\":" + JsonQuote(ui::ToString(node->role)) +
                     ",\"name\":" +
                     JsonQuote(node->GetStringAttribute(
                         ax::mojom::StringAttribute::kName)) +
                     ",\"children\":[";
  for (size_t i = 0; i < node->child_ids.size(); ++i) {
    if (i > 0) {
      json += ",";
    }
    json += RenderNode(by_id, node->child_ids[i]);
  }
  json += "]}";
  return json;
}

}  // namespace

std::string GetAxTreeJson(content::WebContents* wc) {
  if (!wc) {
    return "null";
  }
  std::string result = "null";
  base::RunLoop run_loop;
  wc->RequestAXTreeSnapshot(
      base::BindOnce(
          [](std::string* out, base::RunLoop* loop, ui::AXTreeUpdate& update) {
            std::map<int32_t, const ui::AXNodeData*> by_id;
            for (const ui::AXNodeData& node : update.nodes) {
              by_id[node.id] = &node;
            }
            if (update.root_id != ui::kInvalidAXNodeID) {
              *out = RenderNode(by_id, update.root_id);
            }
            loop->Quit();
          },
          &result, &run_loop),
      ui::kAXModeComplete, /*max_nodes=*/0,
      /*timeout=*/base::Seconds(0),
      content::WebContents::AXTreeSnapshotPolicy::kAll);
  run_loop.Run();
  return result;
}

}  // namespace aurelian
