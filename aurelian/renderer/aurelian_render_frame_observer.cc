// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/renderer/aurelian_render_frame_observer.h"

#include <string>
#include <vector>

#include "base/logging.h"
#include "content/public/renderer/render_frame.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_registry.h"

namespace aurelian {

AurelianRenderFrameObserver::AurelianRenderFrameObserver(
    content::RenderFrame* frame)
    : content::RenderFrameObserver(frame) {
  LOG(WARNING) << "[aurelian-c2] renderer: AurelianRenderFrameObserver created";
  frame->GetAssociatedInterfaceRegistry()
      ->AddInterface<aurelian::mojom::AurelianWire>(base::BindRepeating(
          &AurelianRenderFrameObserver::BindAurelianWire,
          base::Unretained(this)));
}

AurelianRenderFrameObserver::~AurelianRenderFrameObserver() = default;

void AurelianRenderFrameObserver::OnDestruct() {
  delete this;
}

void AurelianRenderFrameObserver::BindAurelianWire(
    mojo::PendingAssociatedReceiver<aurelian::mojom::AurelianWire> receiver) {
  LOG(WARNING) << "[aurelian-c2] renderer: BindAurelianWire called";
  receiver_.reset();
  receiver_.Bind(std::move(receiver));
}

void AurelianRenderFrameObserver::Dispatch(
    const std::vector<uint8_t>& envelope,
    DispatchCallback callback) {
  std::string verb(envelope.begin(), envelope.end());
  LOG(WARNING) << "[aurelian-c2] renderer received Dispatch: " << verb;

  std::string reply_str;
  if (verb == "describe") {
    // Return a simple identity proving we're in the renderer.
    reply_str = "{\"origin\":\"renderer\"}";
  } else if (verb == "__getIdentity") {
    reply_str = "legion://chrome/renderer/frame";
  } else {
    reply_str = "{\"error\":\"not-callable\"}";
  }

  std::vector<uint8_t> reply(reply_str.begin(), reply_str.end());
  std::move(callback).Run(reply);
}

}  // namespace aurelian
