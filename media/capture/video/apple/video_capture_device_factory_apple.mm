// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/capture/video/apple/video_capture_device_factory_apple.h"

#include <dirent.h>
#include <stddef.h>
#include <sys/stat.h>

#include "media/base/media_switches.h"
#include "media/capture/video/asmodeus/asmodeus_video_capture_device.h"

#include <memory>
#include <utility>

#include "base/command_line.h"
#include "base/compiler_specific.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/strings/string_util.h"
#include "base/strings/sys_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "media/base/mac/video_capture_device_avfoundation_helpers.h"
#include "media/capture/capture_switches.h"
#include "media/capture/video/apple/video_capture_device_apple.h"
#import "media/capture/video/apple/video_capture_device_avfoundation.h"
#import "media/capture/video/apple/video_capture_device_avfoundation_utils.h"

#if BUILDFLAG(IS_MAC)
#import <IOKit/audio/IOAudioTypes.h>

#import "media/capture/video/mac/video_capture_device_decklink_mac.h"
#import "media/capture/video/mac/video_capture_metrics_mac.h"
#endif

BASE_FEATURE(kVideoCaptureDeviceFactoryAppleLogging,
             base::FEATURE_DISABLED_BY_DEFAULT);

namespace {

#if BUILDFLAG(IS_MAC)
void EnsureRunsOnCFRunLoopEnabledThread() {
  static bool has_checked_cfrunloop_for_video_capture = false;
  if (!has_checked_cfrunloop_for_video_capture) {
    base::apple::ScopedCFTypeRef<CFRunLoopMode> mode(
        CFRunLoopCopyCurrentMode(CFRunLoopGetCurrent()));
    CHECK(mode)
        << "The MacOS video capture code must be run on a CFRunLoop-enabled "
           "thread";
    has_checked_cfrunloop_for_video_capture = true;
  }
}
#endif

media::VideoCaptureFormats GetDeviceSupportedFormats(AVCaptureDevice* device) {
  media::VideoCaptureFormats formats;

  for (AVCaptureDeviceFormat* device_format in device.formats) {
    // MediaSubType is a CMPixelFormatType but can be used as CVPixelFormatType
    // as well according to CMFormatDescription.h
    const media::VideoPixelFormat pixelFormat = [VideoCaptureDeviceAVFoundation
        FourCCToChromiumPixelFormat:CMFormatDescriptionGetMediaSubType(
                                        device_format.formatDescription)];

    CMVideoDimensions dimensions =
        CMVideoFormatDescriptionGetDimensions(device_format.formatDescription);

    for (AVFrameRateRange* frameRate in device_format
             .videoSupportedFrameRateRanges) {
      media::VideoCaptureFormat format(
          gfx::Size(dimensions.width, dimensions.height),
          frameRate.maxFrameRate, pixelFormat);
      DVLOG(2) << device.localizedName << " "
               << media::VideoCaptureFormat::ToString(format);
      formats.push_back(std::move(format));
    }
  }
  return formats;
}

// Blocked devices are identified by a characteristic trailing substring of
// uniqueId. At the moment these are just Blackmagic devices.
constexpr const char* kBlockedCamerasIdSignature[] = {"-01FDA82C8A9C"};

bool IsDeviceBlockedForAVFoundation(const std::string& device_id) {
  bool is_device_blocked = false;
  for (size_t i = 0;
       !is_device_blocked && i < std::size(kBlockedCamerasIdSignature); ++i) {
    is_device_blocked =
        base::EndsWith(device_id, UNSAFE_TODO(kBlockedCamerasIdSignature[i]),
                       base::CompareCase::INSENSITIVE_ASCII);
  }
  return is_device_blocked;
}

bool IsDeviceBlocked(const media::VideoCaptureDeviceDescriptor& descriptor) {
  bool is_device_blocked = IsDeviceBlockedForAVFoundation(descriptor.device_id);
  DVLOG_IF(2, is_device_blocked)
      << "Blocked camera: " << descriptor.display_name()
      << ", id: " << descriptor.device_id;
  return is_device_blocked;
}

// TODO(crbug.com/436126054) remove these functions after issue is resolved.
void API_AVAILABLE(macos(14.0)) ListAvailableCaptureDevices() {
  NSArray* deviceTypes = @[
    AVCaptureDeviceTypeBuiltInWideAngleCamera,
    AVCaptureDeviceTypeContinuityCamera, AVCaptureDeviceTypeExternal,
    AVCaptureDeviceTypeMicrophone
  ];

  AVCaptureDeviceDiscoverySession* deviceDiscoverySession =
      [AVCaptureDeviceDiscoverySession
          discoverySessionWithDeviceTypes:deviceTypes
                                mediaType:nil
                                 position:AVCaptureDevicePositionUnspecified];

  if ([deviceDiscoverySession.devices count] == 0) {
    LOG(ERROR) << "Simple Query - No AVCaptureDevices found.";
  } else {
    for (AVCaptureDevice* device in deviceDiscoverySession.devices) {
      LOG(ERROR) << "Simple Query - Device Name:"
                 << base::SysNSStringToUTF8(device.localizedName)
                 << " Unique ID:" << base::SysNSStringToUTF8(device.uniqueID)
                 << " Model ID:" << base::SysNSStringToUTF8(device.modelID)
                 << " Type:" << base::SysNSStringToUTF8(device.deviceType)
                 << " Position:" << [device position];
    }
  }
}

// TODO(crbug.com/436126054) remove these functions after issue is resolved.
void API_AVAILABLE(macos(14.0)) ListAvailableCaptureDevicesNoMic() {
  NSArray* deviceTypes = @[
    AVCaptureDeviceTypeBuiltInWideAngleCamera,
    AVCaptureDeviceTypeContinuityCamera, AVCaptureDeviceTypeExternal
  ];

  AVCaptureDeviceDiscoverySession* deviceDiscoverySession =
      [AVCaptureDeviceDiscoverySession
          discoverySessionWithDeviceTypes:deviceTypes
                                mediaType:nil
                                 position:AVCaptureDevicePositionUnspecified];

  if ([deviceDiscoverySession.devices count] == 0) {
    LOG(ERROR) << "Simple Query NoMic - No AVCaptureDevices found.";
  } else {
    for (AVCaptureDevice* device in deviceDiscoverySession.devices) {
      LOG(ERROR) << "Simple Query NoMic - Device Name:"
                 << base::SysNSStringToUTF8(device.localizedName)
                 << " Unique ID:" << base::SysNSStringToUTF8(device.uniqueID)
                 << " Model ID:" << base::SysNSStringToUTF8(device.modelID)
                 << " Type:" << base::SysNSStringToUTF8(device.deviceType)
                 << " Position:" << [device position];
    }
  }
}

// TODO(crbug.com/436126054) remove these functions after issue is resolved.
void API_AVAILABLE(macos(14.0)) ListAvailableCaptureDevicesMediaType() {
  NSArray* deviceTypes = @[
    AVCaptureDeviceTypeBuiltInWideAngleCamera,
    AVCaptureDeviceTypeContinuityCamera, AVCaptureDeviceTypeExternal
  ];

  AVCaptureDeviceDiscoverySession* deviceDiscoverySession =
      [AVCaptureDeviceDiscoverySession
          discoverySessionWithDeviceTypes:deviceTypes
                                mediaType:AVMediaTypeVideo
                                 position:AVCaptureDevicePositionUnspecified];

  if ([deviceDiscoverySession.devices count] == 0) {
    LOG(ERROR) << "Simple Query MediaType - No AVCaptureDevices found.";
  } else {
    for (AVCaptureDevice* device in deviceDiscoverySession.devices) {
      LOG(ERROR) << "Simple Query MediaType - Device Name:"
                 << base::SysNSStringToUTF8(device.localizedName)
                 << " Unique ID:" << base::SysNSStringToUTF8(device.uniqueID)
                 << " Model ID:" << base::SysNSStringToUTF8(device.modelID)
                 << " Type:" << base::SysNSStringToUTF8(device.deviceType)
                 << " Position:" << [device position];
    }
  }
}

}  // anonymous namespace

namespace media {

VideoCaptureDeviceFactoryApple::VideoCaptureDeviceFactoryApple() {
  thread_checker_.DetachFromThread();
}

VideoCaptureErrorOrDevice VideoCaptureDeviceFactoryApple::CreateDevice(
    const VideoCaptureDeviceDescriptor& descriptor) {
  DCHECK(thread_checker_.CalledOnValidThread());
  DCHECK_NE(descriptor.capture_api, VideoCaptureApi::UNKNOWN);

#if BUILDFLAG(IS_MAC)
  EnsureRunsOnCFRunLoopEnabledThread();
#endif

  std::unique_ptr<VideoCaptureDevice> capture_device;

  // Asmodeus virtual camera
  if (descriptor.capture_api == VideoCaptureApi::VIRTUAL_DEVICE &&
      descriptor.device_id.substr(0, 13) == "asmodeus-cam-") {
    std::string name = descriptor.device_id.substr(13);
    const char* home = getenv("HOME");
    std::string shm_path = std::string(home ? home : "/tmp") +
        "/.asmodeus/video-in-" + name + ".shm";
    capture_device = std::make_unique<asmodeus::AsmodeusVideoCaptureDevice>(
        name, shm_path);
    return VideoCaptureErrorOrDevice(std::move(capture_device));
  }

  if (descriptor.capture_api != VideoCaptureApi::MACOSX_DECKLINK) {
    VideoCaptureDeviceApple* device = new VideoCaptureDeviceApple(descriptor);
    capture_device.reset(device);
    if (!device->Init(descriptor.capture_api)) {
      LOG(ERROR) << "Could not initialize VideoCaptureDevice.";
      capture_device.reset();
    }
  }
#if BUILDFLAG(IS_MAC)
  else {
    capture_device =
        std::make_unique<VideoCaptureDeviceDeckLinkMac>(descriptor);
  }
#endif

#if BUILDFLAG(IS_MAC)
  if (capture_device) {
    LogReactionEffectsGesturesState();
  }
#endif

  return capture_device ? VideoCaptureErrorOrDevice(std::move(capture_device))
                        : VideoCaptureErrorOrDevice(
                              VideoCaptureError::kMacSetCaptureDeviceFailed);
}

void VideoCaptureDeviceFactoryApple::GetDevicesInfo(
    GetDevicesInfoCallback callback) {
  DCHECK(thread_checker_.CalledOnValidThread());

#if BUILDFLAG(IS_MAC)
  EnsureRunsOnCFRunLoopEnabledThread();
#endif

  NSArray<AVCaptureDevice*>* devices = media::GetVideoCaptureDevices();

  // Loop through all available devices and add to |devices_info|.
  std::vector<VideoCaptureDeviceInfo> devices_info;
  DVLOG(1) << "Enumerating video capture devices using AVFoundation";

  const bool debug_logging_enabled =
      base::FeatureList::IsEnabled(kVideoCaptureDeviceFactoryAppleLogging);

#if BUILDFLAG(IS_IOS)
  bool default_set = false;
#endif
  // available() must be in it's own separate if statement.
  if (@available(macOS 14.0, *)) {
    if (debug_logging_enabled) {
      ListAvailableCaptureDevices();
      ListAvailableCaptureDevicesNoMic();
      ListAvailableCaptureDevicesMediaType();
    }
  }

  for (AVCaptureDevice* device in devices) {
    if ([device hasMediaType:AVMediaTypeVideo] ||
        [device hasMediaType:AVMediaTypeMuxed]) {
      if (debug_logging_enabled) {
        LOG(ERROR) << "\ndevice: "
                   << base::SysNSStringToUTF8(device.localizedName) << "\n"
                   << "id: " << base::SysNSStringToUTF8(device.uniqueID) << "\n"
#if BUILDFLAG(IS_MAC)
                   << "type: " << device.transportType << "\n"
#endif
                   << "suspended: " << (device.suspended ? "true" : "false");
      }

      if (device.suspended) {
        continue;
      }

      const std::string device_id = base::SysNSStringToUTF8(device.uniqueID);
      const VideoCaptureApi capture_api = VideoCaptureApi::MACOSX_AVFOUNDATION;
#if BUILDFLAG(IS_MAC)
      // Transport types are defined for Audio devices and reused for video.
      int transport_type = device.transportType;
      VideoCaptureTransportType device_transport_type =
          (transport_type == kIOAudioDeviceTransportTypeBuiltIn ||
           transport_type == kIOAudioDeviceTransportTypeUSB)
              ? VideoCaptureTransportType::APPLE_USB_OR_BUILT_IN
              : VideoCaptureTransportType::OTHER_TRANSPORT;
#else
      VideoCaptureTransportType device_transport_type =
          VideoCaptureTransportType::APPLE_USB_OR_BUILT_IN;
#endif
      const std::string model_id = VideoCaptureDeviceApple::GetDeviceModelId(
          device_id, capture_api, device_transport_type);
      const VideoCaptureControlSupport control_support =
          VideoCaptureDeviceApple::GetControlSupport(model_id);
      VideoCaptureDeviceDescriptor descriptor(
          base::SysNSStringToUTF8(device.localizedName), device_id, model_id,
          capture_api, control_support, device_transport_type);
      if (IsDeviceBlocked(descriptor)) {
        if (debug_logging_enabled) {
          LOG(ERROR) << "Device is blocklisted";
        }
        continue;
      }

      VideoCaptureDeviceInfo device_info(descriptor);
      // Get supported formats
      device_info.supported_formats = GetDeviceSupportedFormats(device);
      if (debug_logging_enabled) {
        LOG(ERROR) << "supported formats: "
                   << device_info.supported_formats.size();
      }

#if BUILDFLAG(IS_IOS)
      // Always place the first front facing camera as the default.
      if (!default_set && [device position] == AVCaptureDevicePositionFront) {
        devices_info.insert(devices_info.begin(), std::move(device_info));
        default_set = true;
      } else
#endif
      {
        devices_info.push_back(std::move(device_info));
      }
    }
  }

#if BUILDFLAG(IS_MAC)
  // Also retrieve Blackmagic devices, if present, via DeckLink SDK API.
  VideoCaptureDeviceDeckLinkMac::EnumerateDevices(&devices_info);

  // Asmodeus virtual cameras — discovered by scanning ~/.asmodeus/ for
  // video-in-*.shm files. Filtered by --asmodeus-device flag if set.
  {
    std::string device_filter;
    auto* cmd = base::CommandLine::ForCurrentProcess();
    if (cmd->HasSwitch(switches::kAsmodeusDevice)) {
      device_filter = cmd->GetSwitchValueASCII(switches::kAsmodeusDevice);
    }

    const char* home = getenv("HOME");
    if (home) {
      std::string dir = std::string(home) + "/.asmodeus";
      DIR* d = opendir(dir.c_str());
      if (d) {
        struct dirent* entry;
        while ((entry = readdir(d)) != nullptr) {
          std::string_view filename(entry->d_name);
          // Match video-in-{name}.shm
          if (filename.size() > 14 &&
              filename.substr(0, 9) == "video-in-" &&
              filename.substr(filename.size() - 4) == ".shm") {
            std::string name(filename.substr(9, filename.size() - 13));
            // Filter by --asmodeus-device flag
            if (!device_filter.empty() && name != device_filter) continue;
            std::string path = dir + "/" + std::string(filename);
            struct stat st;
            if (stat(path.c_str(), &st) == 0 && st.st_size > 32) {
              std::string device_id =
                  std::string(asmodeus::AsmodeusVideoCaptureDevice::kDeviceIdPrefix) + name;
              std::string device_name =
                  std::string(asmodeus::AsmodeusVideoCaptureDevice::kDeviceNamePrefix) + name;

              VideoCaptureDeviceDescriptor descriptor(
                  device_name, device_id,
                  VideoCaptureApi::VIRTUAL_DEVICE,
                  VideoCaptureControlSupport(),
                  VideoCaptureTransportType::OTHER_TRANSPORT);

              // Read resolution from shm header.
              // Layout matches native_agent/video_shm.h VideoShmHeader:
              //   [0] width, [1] height, [2] frame_size,
              //   [3] current_buffer, [4] frame_sequence
              int fd = open(path.c_str(), O_RDONLY);
              if (fd >= 0) {
                uint32_t header[5];
                if (read(fd, header, 20) == 20) {
                  int w = static_cast<int>(header[0]);
                  int h = static_cast<int>(header[1]);
                  constexpr int kDefaultFps = 30;
                  if (w > 0 && h > 0) {
                    VideoCaptureDeviceInfo info(descriptor);
                    info.supported_formats.push_back(
                        VideoCaptureFormat(gfx::Size(w, h),
                                          static_cast<float>(kDefaultFps),
                                          PIXEL_FORMAT_NV12));
                    devices_info.push_back(std::move(info));
                    LOG(WARNING) << "[Asmodeus] Virtual camera: " << device_name
                                 << " (" << w << "x" << h << "@" << kDefaultFps
                                 << " seq=" << header[4] << ")";
                  }
                }
                close(fd);
              }
            }
          }
        }
        closedir(d);
      }
    }
  }
#endif
  std::move(callback).Run(std::move(devices_info));
}

bool ShouldEnableGpuMemoryBuffer(const std::string& device_id) {
  // Asmodeus virtual cameras use software buffers, not GPU memory.
  if (device_id.size() > 13 && device_id.substr(0, 13) == "asmodeus-cam-") {
    return false;
  }
  return !base::CommandLine::ForCurrentProcess()->HasSwitch(
             switches::kDisableVideoCaptureUseGpuMemoryBuffer) &&
         !IsDeviceBlockedForAVFoundation(device_id);
}

}  // namespace media
