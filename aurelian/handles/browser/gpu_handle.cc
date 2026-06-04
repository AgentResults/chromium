// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/handles/browser/gpu_handle.h"

#include "content/public/browser/gpu_data_manager.h"
#include "gpu/config/gpu_info.h"

namespace aurelian {

GpuSummary GetGpuSummary() {
  gpu::GPUInfo info = content::GpuDataManager::GetInstance()->GetGPUInfo();
  const gpu::GPUInfo::GPUDevice& active = info.active_gpu();

  GpuSummary s;
  s.vendor_id = active.vendor_id;
  s.device_id = active.device_id;
  s.driver_version = active.driver_version;
  s.gl_vendor = info.gl_vendor;
  s.gl_renderer = info.gl_renderer;
  return s;
}

}  // namespace aurelian
