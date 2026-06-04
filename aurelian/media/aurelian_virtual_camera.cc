// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/media/aurelian_virtual_camera.h"

#include <utility>

#include "content/public/browser/video_capture_service.h"
#include "media/base/video_types.h"
#include "media/capture/video/video_capture_device_info.h"
#include "media/capture/video_capture_types.h"
#include "services/video_capture/public/mojom/video_capture_service.mojom.h"
#include "ui/gfx/geometry/size.h"

namespace aurelian {

const char AurelianVirtualCamera::kDeviceId[] = "/aurelian/camera";
const char AurelianVirtualCamera::kDisplayName[] = "Aurelian Virtual Camera";

AurelianVirtualCamera::AurelianVirtualCamera() = default;
AurelianVirtualCamera::~AurelianVirtualCamera() = default;

void AurelianVirtualCamera::Start() {
  content::GetVideoCaptureService().ConnectToVideoSourceProvider(
      provider_.BindNewPipeAndPassReceiver());
  RegisterDevice();
}

void AurelianVirtualCamera::StartWithProvider(
    mojo::Remote<video_capture::mojom::VideoSourceProvider> provider) {
  provider_ = std::move(provider);
  RegisterDevice();
}

void AurelianVirtualCamera::RegisterDevice() {
  media::VideoCaptureDeviceInfo info;
  info.descriptor.device_id = kDeviceId;
  info.descriptor.set_display_name(kDisplayName);
  info.descriptor.capture_api = media::VideoCaptureApi::VIRTUAL_DEVICE;
  // Advertise one format so consumers know the geometry. The frame pump
  // (C-MEDIA-2b) fills these frames from the avatar in MediaSeam.
  info.supported_formats.emplace_back(gfx::Size(640, 480), 30.0,
                                      media::PIXEL_FORMAT_I420);

  mojo::PendingRemote<video_capture::mojom::Producer> producer;
  producer_receiver_.Bind(producer.InitWithNewPipeAndPassReceiver());
  provider_->AddSharedMemoryVirtualDevice(
      info, std::move(producer),
      virtual_device_.BindNewPipeAndPassReceiver());
  registered_ = true;
}

void AurelianVirtualCamera::FlushForTesting() {
  if (provider_) {
    provider_.FlushForTesting();
  }
  if (virtual_device_) {
    virtual_device_.FlushForTesting();
  }
}

void AurelianVirtualCamera::OnNewBuffer(
    int32_t /*buffer_id*/,
    media::mojom::VideoBufferHandlePtr /*buffer_handle*/,
    OnNewBufferCallback callback) {
  // Buffer mapping + frame writes land with the frame pump (C-MEDIA-2b). The
  // service only allocates buffers once a consumer opens the device.
  std::move(callback).Run();
}

void AurelianVirtualCamera::OnBufferRetired(int32_t /*buffer_id*/) {}

}  // namespace aurelian
