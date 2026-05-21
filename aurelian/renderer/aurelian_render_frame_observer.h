// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AURELIAN_RENDERER_AURELIAN_RENDER_FRAME_OBSERVER_H_
#define AURELIAN_RENDERER_AURELIAN_RENDER_FRAME_OBSERVER_H_

#include "aurelian/public/mojom/aurelian_wire.mojom.h"
#include "content/public/renderer/render_frame.h"
#include "content/public/renderer/render_frame_observer.h"
#include "mojo/public/cpp/bindings/associated_receiver.h"
#include "mojo/public/cpp/bindings/pending_associated_receiver.h"

namespace aurelian {

// Per-frame Mojo handler. Binds AurelianWire as an associated interface
// on the frame so the browser process can Dispatch envelopes to it.
class AurelianRenderFrameObserver
    : public content::RenderFrameObserver,
      public aurelian::mojom::AurelianWire {
 public:
  explicit AurelianRenderFrameObserver(content::RenderFrame* frame);
  ~AurelianRenderFrameObserver() override;

  AurelianRenderFrameObserver(const AurelianRenderFrameObserver&) = delete;
  AurelianRenderFrameObserver& operator=(const AurelianRenderFrameObserver&) =
      delete;

 private:
  // content::RenderFrameObserver:
  void OnDestruct() override;

  // aurelian::mojom::AurelianWire:
  void Dispatch(const std::vector<uint8_t>& envelope,
                DispatchCallback callback) override;

  void BindAurelianWire(
      mojo::PendingAssociatedReceiver<aurelian::mojom::AurelianWire>
          receiver);

  mojo::AssociatedReceiver<aurelian::mojom::AurelianWire> receiver_{this};
};

}  // namespace aurelian

#endif  // AURELIAN_RENDERER_AURELIAN_RENDER_FRAME_OBSERVER_H_
