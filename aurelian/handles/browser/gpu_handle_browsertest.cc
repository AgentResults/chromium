// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C15.b browser test — GPU info reads the browser's real GpuDataManager
// (verified by self-consistency against an independent read, so it holds
// regardless of the test host's GPU or software-rendering fallback).

#include "aurelian/handles/browser/gpu_handle.h"

#include "chrome/test/base/in_process_browser_test.h"
#include "content/public/browser/gpu_data_manager.h"
#include "content/public/test/browser_test.h"
#include "gpu/config/gpu_info.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace aurelian {

using AurelianGpuBrowserTest = InProcessBrowserTest;

IN_PROC_BROWSER_TEST_F(AurelianGpuBrowserTest, ReadsRealGpuInfo) {
  // Independent read of the same source of truth.
  gpu::GPUInfo info = content::GpuDataManager::GetInstance()->GetGPUInfo();

  GpuSummary summary = GetGpuSummary();

  EXPECT_EQ(summary.vendor_id, info.active_gpu().vendor_id);
  EXPECT_EQ(summary.device_id, info.active_gpu().device_id);
  EXPECT_EQ(summary.driver_version, info.active_gpu().driver_version);
  EXPECT_EQ(summary.gl_vendor, info.gl_vendor);
  EXPECT_EQ(summary.gl_renderer, info.gl_renderer);
}

}  // namespace aurelian
