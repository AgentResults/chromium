// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AURELIAN_MEDIA_AURELIAN_VIRTUAL_CAMERA_H_
#define AURELIAN_MEDIA_AURELIAN_VIRTUAL_CAMERA_H_

#include <cstdint>

#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/video_capture/public/mojom/producer.mojom.h"
#include "services/video_capture/public/mojom/video_source_provider.mojom.h"
#include "services/video_capture/public/mojom/virtual_device.mojom.h"

namespace aurelian {

// AurelianVirtualCamera — registers a REAL, enumerable camera device with
// Chromium's video capture service, so the agent's avatar can be surfaced as
// the browser camera (AURELIAN-DESIGN §12 / C-MEDIA-2). Once Start()ed,
// navigator.mediaDevices.enumerateDevices() in any page lists an Aurelian
// videoinput device, and getUserMedia({video}) can open it — the honest,
// browser-process virtual-device seam (NOT a renderer permission fake).
//
// This slice (C-MEDIA-2a) establishes the enumerable device. The frame pump
// that fills it from MediaSeam (avatar frames) is C-MEDIA-2b.
class AurelianVirtualCamera : public video_capture::mojom::Producer {
 public:
  static const char kDeviceId[];
  static const char kDisplayName[];

  AurelianVirtualCamera();
  ~AurelianVirtualCamera() override;

  AurelianVirtualCamera(const AurelianVirtualCamera&) = delete;
  AurelianVirtualCamera& operator=(const AurelianVirtualCamera&) = delete;

  // Connects to the video capture service and registers the virtual device.
  // Must run on the UI thread. The device stays registered until this object
  // is destroyed (which drops the producer/device pipes).
  void Start();

  // Registers against an already-connected provider (production wiring / tests
  // that own the provider connection).
  void StartWithProvider(
      mojo::Remote<video_capture::mojom::VideoSourceProvider> provider);

  bool registered() const { return registered_; }

  // Blocks until the service has processed the registration (test-only).
  void FlushForTesting();

  // video_capture::mojom::Producer:
  void OnNewBuffer(int32_t buffer_id,
                   media::mojom::VideoBufferHandlePtr buffer_handle,
                   OnNewBufferCallback callback) override;
  void OnBufferRetired(int32_t buffer_id) override;

 private:
  void RegisterDevice();

  bool registered_ = false;
  mojo::Remote<video_capture::mojom::VideoSourceProvider> provider_;
  mojo::Receiver<video_capture::mojom::Producer> producer_receiver_{this};
  mojo::Remote<video_capture::mojom::SharedMemoryVirtualDevice> virtual_device_;
};

}  // namespace aurelian

#endif  // AURELIAN_MEDIA_AURELIAN_VIRTUAL_CAMERA_H_
