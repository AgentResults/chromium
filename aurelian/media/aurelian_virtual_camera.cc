// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/media/aurelian_virtual_camera.h"

#include <algorithm>
#include <utility>

#include "aurelian/handles/media/media_seam.h"
#include "base/functional/bind.h"
#include "base/memory/unsafe_shared_memory_region.h"
#include "content/public/browser/video_capture_service.h"
#include "media/base/video_frame_metadata.h"
#include "media/base/video_types.h"
#include "media/capture/mojom/video_capture_buffer.mojom.h"
#include "media/capture/mojom/video_capture_types.mojom.h"
#include "media/capture/video/video_capture_device_info.h"
#include "media/capture/video_capture_types.h"
#include "services/video_capture/public/mojom/constants.mojom.h"
#include "services/video_capture/public/mojom/video_capture_service.mojom.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"

namespace aurelian {

namespace {
constexpr int kWidth = 640;
constexpr int kHeight = 480;
// I420: full-res Y + quarter-res U + quarter-res V.
constexpr size_t kI420Size = kWidth * kHeight + 2 * ((kWidth / 2) * (kHeight / 2));
constexpr double kFrameRate = 30.0;
}  // namespace

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
  StartPump();
}

void AurelianVirtualCamera::StartPump() {
  // Drive frames at the advertised rate. RequestFrameBuffer returns an invalid
  // id while no consumer has the device open, so this is a harmless no-op until
  // getUserMedia (or any consumer) opens the Aurelian camera.
  pump_timer_.Start(FROM_HERE, base::Hertz(kFrameRate), this,
                    &AurelianVirtualCamera::PushNextFrame);
}

void AurelianVirtualCamera::PushNextFrame() {
  if (!virtual_device_) {
    return;
  }
  const base::TimeDelta timestamp = (frame_count_++) / kFrameRate * base::Seconds(1);
  virtual_device_->RequestFrameBuffer(
      gfx::Size(kWidth, kHeight), media::PIXEL_FORMAT_I420, /*strides=*/nullptr,
      base::BindOnce(&AurelianVirtualCamera::OnFrameBufferReceived,
                     weak_factory_.GetWeakPtr(), timestamp));
}

void AurelianVirtualCamera::OnFrameBufferReceived(base::TimeDelta timestamp,
                                                  int32_t buffer_id) {
  if (buffer_id == video_capture::mojom::kInvalidBufferId) {
    return;
  }
  auto it = buffers_.find(buffer_id);
  if (it == buffers_.end()) {
    return;
  }
  base::span<uint8_t> dest = it->second.GetMemoryAsSpan<uint8_t>();
  if (dest.size() < kI420Size) {
    return;
  }

  // Fill the buffer with the latest avatar frame from MediaSeam. A correctly
  // sized I420 avatar frame is copied verbatim; otherwise a neutral gray frame
  // (Y/U/V = 128) keeps the device producing valid frames until the avatar
  // arrives.
  InjectedVideoFrame frame = MediaSeam::Get().LatestVideoFrame();
  if (frame.valid && frame.fmt == "I420" && frame.pixels.size() == kI420Size) {
    dest.first(kI420Size).copy_from(base::span(frame.pixels));
  } else {
    std::fill(dest.begin(), dest.begin() + kI420Size, uint8_t{128});
  }

  media::VideoFrameMetadata metadata;
  metadata.frame_rate = kFrameRate;
  metadata.reference_time = base::TimeTicks::Now();

  auto info = media::mojom::VideoFrameInfo::New();
  info->timestamp = timestamp;
  info->pixel_format = media::PIXEL_FORMAT_I420;
  info->coded_size = gfx::Size(kWidth, kHeight);
  info->visible_rect = gfx::Rect(0, 0, kWidth, kHeight);
  info->metadata = metadata;

  virtual_device_->OnFrameReadyInBuffer(buffer_id, std::move(info));
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
    int32_t buffer_id,
    media::mojom::VideoBufferHandlePtr buffer_handle,
    OnNewBufferCallback callback) {
  // The service allocates buffers once a consumer opens the device. Map each
  // for writing so the pump can fill it with avatar pixels.
  if (buffer_handle->is_unsafe_shmem_region()) {
    base::WritableSharedMemoryMapping mapping =
        std::move(buffer_handle->get_unsafe_shmem_region()).Map();
    if (mapping.IsValid()) {
      buffers_.insert({buffer_id, std::move(mapping)});
    }
  }
  std::move(callback).Run();
}

void AurelianVirtualCamera::OnBufferRetired(int32_t buffer_id) {
  buffers_.erase(buffer_id);
}

}  // namespace aurelian
