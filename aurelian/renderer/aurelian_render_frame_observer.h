// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AURELIAN_RENDERER_AURELIAN_RENDER_FRAME_OBSERVER_H_
#define AURELIAN_RENDERER_AURELIAN_RENDER_FRAME_OBSERVER_H_

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "aurelian/capability/cap_chain.h"
#include "aurelian/public/mojom/aurelian_wire.mojom.h"
#include "base/memory/weak_ptr.h"
#include "content/public/renderer/render_frame.h"
#include "content/public/renderer/render_frame_observer.h"
#include "mojo/public/cpp/bindings/associated_receiver.h"
#include "ui/base/page_transition_types.h"
#include "mojo/public/cpp/bindings/pending_associated_receiver.h"

namespace blink {
class WebElement;
}

namespace aurelian {

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
  void DidCommitProvisionalLoad(ui::PageTransition transition) override;

  // aurelian::mojom::AurelianWire:
  void Dispatch(const std::vector<uint8_t>& envelope,
                DispatchCallback callback) override;
  void Subscribe(const std::vector<uint8_t>& envelope,
                 SubscribeCallback callback) override;
  void SetTrustAnchor(const std::vector<uint8_t>& anchor_pub) override;

  // Verifies a cap chain against this membrane's trust anchor and enforces the
  // effective predicate against `verb`/`target`. Returns an empty string if the
  // op is permitted, or a JSON broken reason if it is denied.
  std::string CheckCap(const std::vector<CapLink>& chain,
                       const std::string& verb,
                       const std::string& target);

  void BindAurelianWire(
      mojo::PendingAssociatedReceiver<aurelian::mojom::AurelianWire> receiver);

  // Dispatch a verb+param to the DOM/JS handlers. Returns reply string.
  std::string DispatchVerb(const std::string& verb, const std::string& param);

  // Evaluates a JS expression in the main world and serializes the result
  // (shared by js.eval + js.callFunction).
  std::string EvalJsExpression(const std::string& expr);

  // Ensure a node is accessible from JS via __aurelian_nodes[id].
  void EnsureNodeBridge(int node_id, const blink::WebElement& el);

  // C6 subscription stream state (one VeliteSink Remote per subscription).
  struct RendererStream;
  void EmitTestFrame(RendererStream* stream);
  void DrainMutationFrames(RendererStream* stream);
  void DrainConsoleFrames(RendererStream* stream);
  void DrainEventFrames(RendererStream* stream);
  // Splits `joined` (frames separated by \x01) and emits one VeliteSink Frame
  // per non-empty piece. Shared by the mutation + console drains.
  void EmitJoinedFrames(RendererStream* stream, const std::string& joined);
  void OnStreamDisconnect(RendererStream* stream);
  // Runs `js` in the main world and returns its string result ("" otherwise).
  std::string EvalString(const std::string& js);

  mojo::AssociatedReceiver<aurelian::mojom::AurelianWire> receiver_{this};
  // Operator trust anchor for this membrane (C8). Set via SetTrustAnchor.
  PubKey trusted_anchor_{};
  bool has_anchor_ = false;
  // Node registry — opaque, defined in .cc.
  struct NodeRegistryImpl;
  std::unique_ptr<NodeRegistryImpl> registry_;
  std::vector<std::unique_ptr<RendererStream>> streams_;

  base::WeakPtrFactory<AurelianRenderFrameObserver> weak_factory_{this};
};

}  // namespace aurelian

#endif  // AURELIAN_RENDERER_AURELIAN_RENDER_FRAME_OBSERVER_H_
