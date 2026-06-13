// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/peerconnection/media_stream_remote_video_source.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <utility>

#include "base/command_line.h"
#include "base/compiler_specific.h"
#include "base/functional/callback_forward.h"
#include "base/logging.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/functional/callback_helpers.h"
#include "base/location.h"
#include "base/memory/raw_ptr.h"
#include "base/metrics/histogram_functions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "base/trace_event/trace_event.h"
#include "media/base/media_switches.h"
#include "media/base/timestamp_constants.h"
#include "media/base/video_color_space.h"
#include "media/base/video_frame.h"
#include "media/base/video_util.h"
#include "third_party/blink/public/common/features.h"
#include "third_party/blink/public/mojom/mediastream/media_stream.mojom-blink.h"
#include "third_party/blink/renderer/platform/scheduler/public/post_cross_thread_task.h"
#include "third_party/blink/renderer/platform/webrtc/convert_to_webrtc_video_frame_buffer.h"
#include "third_party/blink/renderer/platform/webrtc/track_observer.h"
#include "third_party/blink/renderer/platform/webrtc/webrtc_video_frame_adapter.h"
#include "third_party/blink/renderer/platform/webrtc/webrtc_video_utils.h"
#include "third_party/blink/renderer/platform/wtf/cross_thread_copier_base.h"
#include "third_party/blink/renderer/platform/wtf/cross_thread_functional.h"
#include "third_party/blink/renderer/platform/wtf/functional.h"
#include "third_party/blink/renderer/platform/wtf/thread_safe_ref_counted.h"
#include "third_party/webrtc/api/video/i420_buffer.h"
#include "third_party/webrtc/api/video/recordable_encoded_frame.h"
#include "third_party/webrtc/rtc_base/time_utils.h"
#include "third_party/webrtc/system_wrappers/include/clock.h"

namespace blink {

namespace {

class WebRtcEncodedVideoFrame : public EncodedVideoFrame {
 public:
  explicit WebRtcEncodedVideoFrame(const webrtc::RecordableEncodedFrame& frame)
      : buffer_(frame.encoded_buffer()),
        codec_(WebRtcToMediaVideoCodec(frame.codec())),
        is_key_frame_(frame.is_key_frame()),
        resolution_(frame.resolution().width, frame.resolution().height) {
    if (frame.color_space()) {
      color_space_ = WebRtcToGfxColorSpace(*frame.color_space());
    }
    if (frame.video_rotation()) {
      transformation_ = WebRtcToMediaVideoRotation(*frame.video_rotation());
    }
  }

  base::span<const uint8_t> Data() const override { return *buffer_; }

  media::VideoCodec Codec() const override { return codec_; }

  bool IsKeyFrame() const override { return is_key_frame_; }

  std::optional<gfx::ColorSpace> ColorSpace() const override {
    return color_space_;
  }

  std::optional<media::VideoTransformation> Transformation() const override {
    return transformation_;
  }

  gfx::Size Resolution() const override { return resolution_; }

 private:
  webrtc::scoped_refptr<const webrtc::EncodedImageBufferInterface> buffer_;
  media::VideoCodec codec_;
  bool is_key_frame_;
  std::optional<gfx::ColorSpace> color_space_;
  std::optional<media::VideoTransformation> transformation_;
  gfx::Size resolution_;
};

}  // namespace

// Internal class used for receiving frames from the webrtc track on a
// libjingle thread and forward it to the IO-thread.
class MediaStreamRemoteVideoSource::RemoteVideoSourceDelegate
    : public ThreadSafeRefCounted<RemoteVideoSourceDelegate>,
      public webrtc::VideoSinkInterface<webrtc::VideoFrame>,
      public webrtc::VideoSinkInterface<webrtc::RecordableEncodedFrame> {
 public:
  RemoteVideoSourceDelegate(
      scoped_refptr<base::SequencedTaskRunner> video_task_runner,
      VideoCaptureDeliverFrameCB new_frame_callback,
      EncodedVideoFrameCB encoded_frame_callback,
      VideoCaptureVersionCB capture_version_callback);

  void SetHasSeenScreencastContentTypeCallback(base::OnceClosure callback);

 protected:
  friend class ThreadSafeRefCounted<RemoteVideoSourceDelegate>;
  ~RemoteVideoSourceDelegate() override;

  // Implements webrtc::VideoSinkInterface used for receiving video frames
  // from the PeerConnection video track. May be called on a libjingle internal
  // thread.
  void OnFrame(const webrtc::VideoFrame& frame) override;

  // VideoSinkInterface<webrtc::RecordableEncodedFrame>
  void OnFrame(const webrtc::RecordableEncodedFrame& frame) override;

  void DoRenderFrameOnIOThread(
      scoped_refptr<media::VideoFrame> video_frame,
      base::TimeTicks estimated_capture_time,
      std::optional<webrtc::VideoContentType> content_type);

 private:
  void OnEncodedVideoFrameOnIO(scoped_refptr<EncodedVideoFrame> frame,
                               base::TimeTicks estimated_capture_time);

  scoped_refptr<base::SequencedTaskRunner> video_task_runner_;

  // |frame_callback_| is accessed on the IO thread.
  VideoCaptureDeliverFrameCB frame_callback_;

  // |encoded_frame_callback_| is accessed on the IO thread.
  EncodedVideoFrameCB encoded_frame_callback_;

  // |capture_version_callback| is accessed on the IO thread.
  VideoCaptureVersionCB capture_version_callback_;

  // Timestamp of the first received frame.
  std::optional<base::TimeTicks> start_timestamp_;

  // WebRTC real time clock, needed to determine NTP offset.
  raw_ptr<webrtc::Clock> clock_;

  // Offset between NTP clock and WebRTC clock.
  const int64_t ntp_offset_;

  // Determined from a feature flag; if set WebRTC won't forward an unspecified
  // color space.
  const bool ignore_unspecified_color_space_;

  // Called with true if frames where ever received here with screenshare
  // content type.
  base::OnceClosure has_seen_screencast_content_type_callback_;

  // AURELIAN-MEDIA-CONTROL §5.3: the inbound-video engine hook. Writes the
  // decoded remote frame (this delegate = one remote track) to the
  // participant's ~/.asmodeus/video-out-<name>.shm ring as NV12, in the fork's
  // 20-byte double-buffered layout (w@0, h@4, frame_size@8, current_buffer@12,
  // frame_sequence@16, then 2×frame_size NV12), where <name> is
  // --asmodeus-device. The fork's existing video ring class is reader-only and
  // its native-agent writer is browser-process; this renderer-process writer is
  // the missing piece. Sole producer (SPSC). Lazy-init at the first frame.
  void AsmodeusWriteEye(const webrtc::VideoFrame& frame);
  bool asmodeus_eye_init_done_ = false;
  int asmodeus_eye_fd_ = -1;
  RAW_PTR_EXCLUSION void* asmodeus_eye_map_ = nullptr;
  size_t asmodeus_eye_map_size_ = 0;
  int asmodeus_eye_w_ = 0;
  int asmodeus_eye_h_ = 0;
  size_t asmodeus_eye_frame_size_ = 0;
};

MediaStreamRemoteVideoSource::RemoteVideoSourceDelegate::
    RemoteVideoSourceDelegate(
        scoped_refptr<base::SequencedTaskRunner> video_task_runner,
        VideoCaptureDeliverFrameCB new_frame_callback,
        EncodedVideoFrameCB encoded_frame_callback,
        VideoCaptureVersionCB capture_version_callback)
    : video_task_runner_(video_task_runner),
      frame_callback_(std::move(new_frame_callback)),
      encoded_frame_callback_(std::move(encoded_frame_callback)),
      capture_version_callback_(std::move(capture_version_callback)),
      clock_(webrtc::Clock::GetRealTimeClock()),
      ntp_offset_(clock_->TimeInMilliseconds() -
                  clock_->CurrentNtpInMilliseconds()),
      ignore_unspecified_color_space_(base::FeatureList::IsEnabled(
          features::kWebRtcIgnoreUnspecifiedColorSpace)) {}

MediaStreamRemoteVideoSource::RemoteVideoSourceDelegate::
    ~RemoteVideoSourceDelegate() = default;

void MediaStreamRemoteVideoSource::RemoteVideoSourceDelegate::AsmodeusWriteEye(
    const webrtc::VideoFrame& frame) {
  if (!asmodeus_eye_init_done_) {
    asmodeus_eye_init_done_ = true;
    auto* cmd = base::CommandLine::ForCurrentProcess();
    if (!cmd->HasSwitch("asmodeus-device")) {
      return;
    }
    std::string name = cmd->GetSwitchValueASCII("asmodeus-device");
    const char* home = getenv("HOME");
    const int w = frame.width();
    const int h = frame.height();
    if (name.empty() || !home || w <= 0 || h <= 0 || (w & 1) || (h & 1)) {
      return;
    }
    asmodeus_eye_w_ = w;
    asmodeus_eye_h_ = h;
    asmodeus_eye_frame_size_ = static_cast<size_t>(w) * h * 3 / 2;
    std::string shm_path =
        std::string(home) + "/.asmodeus/video-out-" + name + ".shm";
    size_t map_size = 20 + 2 * asmodeus_eye_frame_size_;
    int fd = open(shm_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
      LOG(ERROR) << "[Asmodeus] inbound-video hook: cannot open " << shm_path;
      return;
    }
    if (ftruncate(fd, static_cast<off_t>(map_size)) != 0) {
      close(fd);
      return;
    }
    void* ptr =
        mmap(nullptr, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
      close(fd);
      return;
    }
    asmodeus_eye_fd_ = fd;
    asmodeus_eye_map_ = ptr;
    asmodeus_eye_map_size_ = map_size;
    UNSAFE_BUFFERS({
      uint32_t* hdr = static_cast<uint32_t*>(ptr);
      hdr[0] = static_cast<uint32_t>(w);
      hdr[1] = static_cast<uint32_t>(h);
      hdr[2] = static_cast<uint32_t>(asmodeus_eye_frame_size_);
      __atomic_store_n(&hdr[3], 0u, __ATOMIC_RELAXED);  // current_buffer
      __atomic_store_n(&hdr[4], 0u, __ATOMIC_RELAXED);  // frame_sequence
      memset(static_cast<uint8_t*>(ptr) + 20, 0, 2 * asmodeus_eye_frame_size_);
    });
    LOG(WARNING) << "[Asmodeus] inbound-video engine hook → " << shm_path << " "
                 << w << "x" << h;
  }
  if (!asmodeus_eye_map_) {
    return;
  }
  // Dimensions fixed at init; a mid-stream size change would need a new ring.
  if (frame.width() != asmodeus_eye_w_ || frame.height() != asmodeus_eye_h_) {
    return;
  }
  webrtc::scoped_refptr<webrtc::I420BufferInterface> i420 =
      frame.video_frame_buffer()->ToI420();
  if (!i420) {
    LOG(ERROR) << "[Asmodeus] inbound-video hook: ToI420 failed, skip frame";
    return;  // conversion failure → loud-skip, never a torn write
  }
  const int w = asmodeus_eye_w_;
  const int h = asmodeus_eye_h_;
  UNSAFE_BUFFERS({
    uint32_t* hdr = static_cast<uint32_t*>(asmodeus_eye_map_);
    uint32_t front = __atomic_load_n(&hdr[3], __ATOMIC_RELAXED);
    uint32_t back = front ^ 1u;
    uint8_t* dst = static_cast<uint8_t*>(asmodeus_eye_map_) + 20 +
                   static_cast<size_t>(back) * asmodeus_eye_frame_size_;
    // Y plane (stride-correct copy).
    const uint8_t* sy = i420->DataY();
    const int stride_y = i420->StrideY();
    for (int y = 0; y < h; ++y) {
      memcpy(dst + static_cast<size_t>(y) * w,
             sy + static_cast<size_t>(y) * stride_y, static_cast<size_t>(w));
    }
    // Interleaved UV plane (NV12: U,V per chroma sample) from I420 U/V planes.
    uint8_t* uv = dst + static_cast<size_t>(w) * h;
    const uint8_t* su = i420->DataU();
    const uint8_t* sv = i420->DataV();
    const int stride_u = i420->StrideU();
    const int stride_v = i420->StrideV();
    const int cw = w / 2;
    const int ch = h / 2;
    for (int y = 0; y < ch; ++y) {
      uint8_t* uv_row = uv + static_cast<size_t>(y) * w;
      const uint8_t* su_row = su + static_cast<size_t>(y) * stride_u;
      const uint8_t* sv_row = sv + static_cast<size_t>(y) * stride_v;
      for (int x = 0; x < cw; ++x) {
        uv_row[2 * x] = su_row[x];
        uv_row[2 * x + 1] = sv_row[x];
      }
    }
    __atomic_store_n(&hdr[3], back, __ATOMIC_RELEASE);
    __atomic_fetch_add(&hdr[4], 1u, __ATOMIC_RELEASE);
  });
}

void MediaStreamRemoteVideoSource::RemoteVideoSourceDelegate::OnFrame(
    const webrtc::VideoFrame& incoming_frame) {
  // AURELIAN-MEDIA-CONTROL §5.3: capture the decoded remote frame at the engine.
  AsmodeusWriteEye(incoming_frame);

  const webrtc::VideoFrame::RenderParameters render_parameters =
      incoming_frame.render_parameters();
  const bool render_immediately = render_parameters.use_low_latency_rendering ||
                                  incoming_frame.timestamp_us() == 0;

  const base::TimeTicks current_time = base::TimeTicks::Now();
  const base::TimeTicks render_time =
      render_immediately
          ? current_time
          : base::TimeTicks() +
                base::Microseconds(incoming_frame.timestamp_us());
  if (!start_timestamp_)
    start_timestamp_ = render_time;
  const base::TimeDelta elapsed_timestamp = render_time - *start_timestamp_;
  TRACE_EVENT2("webrtc", "RemoteVideoSourceDelegate::RenderFrame",
               "Ideal Render Instant", render_time.ToInternalValue(),
               "Timestamp", elapsed_timestamp.InMicroseconds());

  webrtc::scoped_refptr<webrtc::VideoFrameBuffer> buffer =
      incoming_frame.video_frame_buffer();
  scoped_refptr<media::VideoFrame> video_frame;
  if (buffer->type() == webrtc::VideoFrameBuffer::Type::kNative) {
    video_frame = static_cast<WebRtcVideoFrameAdapterInterface*>(buffer.get())
                      ->getMediaVideoFrame();
    video_frame->set_timestamp(elapsed_timestamp);
  } else {
    video_frame =
        ConvertFromMappedWebRtcVideoFrameBuffer(buffer, elapsed_timestamp);
  }
  if (!video_frame)
    return;

  // Rotation may be explicitly set sometimes.
  if (incoming_frame.rotation() != webrtc::kVideoRotation_0) {
    video_frame->metadata().transformation =
        WebRtcToMediaVideoRotation(incoming_frame.rotation());
  }

  // The third clause of the condition is controlled by the feature flag
  // WebRtcIgnoreUnspecifiedColorSpace. If the feature is enabled we won't try
  // to guess a color space if the webrtc::ColorSpace is unspecified. If the
  // feature is disabled (default), an unspecified color space will get
  // converted into a gfx::ColorSpace set to BT601.
  if (!video_frame->ColorSpace().IsValid() && incoming_frame.color_space() &&
      !(ignore_unspecified_color_space_ &&
        incoming_frame.color_space()->primaries() ==
            webrtc::ColorSpace::PrimaryID::kUnspecified &&
        incoming_frame.color_space()->transfer() ==
            webrtc::ColorSpace::TransferID::kUnspecified &&
        incoming_frame.color_space()->matrix() ==
            webrtc::ColorSpace::MatrixID::kUnspecified)) {
    gfx::ColorSpace color_space =
        WebRtcToGfxColorSpace(*incoming_frame.color_space());
    if (!color_space.IsValid()) {
      color_space = media::VideoColorSpace::FromGfxColorSpace(color_space)
                        .GuessGfxColorSpace();
    }
    if (color_space.IsValid()) {
      video_frame->set_color_space(color_space);
    }
  }
  if (base::FeatureList::IsEnabled(media::kWebRTCColorAccuracy) &&
      !video_frame->ColorSpace().IsValid()) {
    video_frame->set_color_space(gfx::ColorSpace::CreateREC601());
  }

  // Run render smoothness algorithm only when we don't have to render
  // immediately.
  if (!render_immediately)
    video_frame->metadata().reference_time = render_time;

  if (render_parameters.max_composition_delay_in_frames) {
    video_frame->metadata().maximum_composition_delay_in_frames =
        render_parameters.max_composition_delay_in_frames;
  }

  video_frame->metadata().decode_end_time = current_time;

  // RTP_TIMESTAMP, PROCESSING_TIME, and CAPTURE_BEGIN_TIME are all exposed
  // through the JavaScript callback mechanism
  // video.requestVideoFrameCallback().
  video_frame->metadata().rtp_timestamp =
      static_cast<double>(incoming_frame.rtp_timestamp());

  if (incoming_frame.processing_time()) {
    video_frame->metadata().processing_time =
        base::Microseconds(incoming_frame.processing_time()->Elapsed().us());
  }

  // Set capture time to the NTP time, which is the estimated capture time
  // converted to the local clock.
  if (incoming_frame.ntp_time_ms() > 0) {
    video_frame->metadata().capture_begin_time =
        base::TimeTicks() +
        base::Milliseconds(incoming_frame.ntp_time_ms() + ntp_offset_);
  }

  // Set receive time to arrival of last packet.
  if (!incoming_frame.packet_infos().empty()) {
    webrtc::Timestamp last_packet_arrival =
        std::max_element(
            incoming_frame.packet_infos().cbegin(),
            incoming_frame.packet_infos().cend(),
            [](const webrtc::RtpPacketInfo& a, const webrtc::RtpPacketInfo& b) {
              return a.receive_time() < b.receive_time();
            })
            ->receive_time();
    video_frame->metadata().receive_time =
        base::TimeTicks() + base::Microseconds(last_packet_arrival.us());
    base::UmaHistogramTimes(
        "WebRTC.Video.TotalReceiveDelay",
        current_time - *video_frame->metadata().receive_time);
  }

  // Use our computed render time as estimated capture time. If timestamp_us()
  // (which is actually the suggested render time) is set by WebRTC, it's based
  // on the RTP timestamps in the frame's packets, so congruent with the
  // received frame capture timestamps. If set by us, it's as congruent as we
  // can get with the timestamp sequence of frames we received.
  PostCrossThreadTask(
      *video_task_runner_, FROM_HERE,
      CrossThreadBindOnce(&RemoteVideoSourceDelegate::DoRenderFrameOnIOThread,
                          WrapRefCounted(this), video_frame, render_time,
                          incoming_frame.content_type()));
}

void MediaStreamRemoteVideoSource::RemoteVideoSourceDelegate::
    DoRenderFrameOnIOThread(
        scoped_refptr<media::VideoFrame> video_frame,
        base::TimeTicks estimated_capture_time,
        std::optional<webrtc::VideoContentType> content_type) {
  TRACE_EVENT0("webrtc", "RemoteVideoSourceDelegate::DoRenderFrameOnIOThread");
  DCHECK(video_task_runner_->RunsTasksInCurrentSequence());
  if (content_type.has_value() &&
      *content_type == webrtc::VideoContentType::SCREENSHARE &&
      has_seen_screencast_content_type_callback_) {
    std::move(has_seen_screencast_content_type_callback_).Run();
  }
  frame_callback_.Run(std::move(video_frame), estimated_capture_time);
}

void MediaStreamRemoteVideoSource::RemoteVideoSourceDelegate::OnFrame(
    const webrtc::RecordableEncodedFrame& frame) {
  const bool render_immediately = frame.render_time().us() == 0;
  const base::TimeTicks current_time = base::TimeTicks::Now();
  const base::TimeTicks render_time =
      render_immediately
          ? current_time
          : base::TimeTicks() + base::Microseconds(frame.render_time().us());

  // Use our computed render time as estimated capture time. If render_time()
  // is set by WebRTC, it's based on the RTP timestamps in the frame's packets,
  // so congruent with the received frame capture timestamps. If set by us, it's
  // as congruent as we can get with the timestamp sequence of frames we
  // received.
  PostCrossThreadTask(
      *video_task_runner_, FROM_HERE,
      CrossThreadBindOnce(&RemoteVideoSourceDelegate::OnEncodedVideoFrameOnIO,
                          WrapRefCounted(this),
                          base::MakeRefCounted<WebRtcEncodedVideoFrame>(frame),
                          render_time));
}

void MediaStreamRemoteVideoSource::RemoteVideoSourceDelegate::
    OnEncodedVideoFrameOnIO(scoped_refptr<EncodedVideoFrame> frame,
                            base::TimeTicks estimated_capture_time) {
  DCHECK(video_task_runner_->RunsTasksInCurrentSequence());
  encoded_frame_callback_.Run(std::move(frame), estimated_capture_time);
}

void MediaStreamRemoteVideoSource::RemoteVideoSourceDelegate::
    SetHasSeenScreencastContentTypeCallback(base::OnceClosure callback) {
  DCHECK(video_task_runner_->RunsTasksInCurrentSequence());
  has_seen_screencast_content_type_callback_ = std::move(callback);
}

MediaStreamRemoteVideoSource::MediaStreamRemoteVideoSource(
    scoped_refptr<base::SingleThreadTaskRunner> task_runner,
    std::unique_ptr<TrackObserver> observer)
    : MediaStreamVideoSource(std::move(task_runner)),
      observer_(std::move(observer)) {
  // The callback will be automatically cleared when 'observer_' goes out of
  // scope and no further callbacks will occur.
  observer_->SetCallback(BindRepeating(&MediaStreamRemoteVideoSource::OnChanged,
                                       Unretained(this)));
}

MediaStreamRemoteVideoSource::~MediaStreamRemoteVideoSource() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DCHECK(!observer_);
}

void MediaStreamRemoteVideoSource::OnSourceTerminated() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  StopSourceImpl();
}

void MediaStreamRemoteVideoSource::StartSourceImpl(
    MediaStreamVideoSourceCallbacks media_stream_callbacks) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DCHECK(!delegate_.get());
  delegate_ = base::MakeRefCounted<RemoteVideoSourceDelegate>(
      video_task_runner(), std::move(media_stream_callbacks.deliver_frame_cb),
      std::move(media_stream_callbacks.encoded_frame_cb),
      std::move(media_stream_callbacks.capture_version_cb));
  scoped_refptr<webrtc::VideoTrackInterface> video_track(
      static_cast<webrtc::VideoTrackInterface*>(observer_->track().get()));
  video_track->AddOrUpdateSink(delegate_.get(), webrtc::VideoSinkWants());
  OnStartDone(mojom::MediaStreamRequestResult::OK);
}

void MediaStreamRemoteVideoSource::StopSourceImpl() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  // StopSourceImpl is called either when MediaStreamTrack.stop is called from
  // JS or blink gc the MediaStreamSource object or when OnSourceTerminated()
  // is called. Garbage collection will happen after the PeerConnection no
  // longer receives the video track.
  if (!observer_)
    return;
  DCHECK(state() != MediaStreamVideoSource::ENDED);
  scoped_refptr<webrtc::VideoTrackInterface> video_track(
      static_cast<webrtc::VideoTrackInterface*>(observer_->track().get()));
  video_track->RemoveSink(delegate_.get());
  // This removes the references to the webrtc video track.
  observer_.reset();
}

webrtc::VideoSinkInterface<webrtc::VideoFrame>*
MediaStreamRemoteVideoSource::SinkInterfaceForTesting() {
  return delegate_.get();
}

webrtc::VideoSinkInterface<webrtc::RecordableEncodedFrame>*
MediaStreamRemoteVideoSource::EncodedSinkInterfaceForTesting() {
  return delegate_.get();
}

void MediaStreamRemoteVideoSource::OnChanged(
    webrtc::MediaStreamTrackInterface::TrackState state) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  switch (state) {
    case webrtc::MediaStreamTrackInterface::kLive:
      SetReadyState(WebMediaStreamSource::kReadyStateLive);
      break;
    case webrtc::MediaStreamTrackInterface::kEnded:
      SetReadyState(WebMediaStreamSource::kReadyStateEnded);
      break;
    default:
      NOTREACHED();
  }
}

bool MediaStreamRemoteVideoSource::SupportsEncodedOutput() const {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  if (!observer_ || !observer_->track()) {
    return false;
  }
  scoped_refptr<webrtc::VideoTrackInterface> video_track(
      static_cast<webrtc::VideoTrackInterface*>(observer_->track().get()));
  return video_track->GetSource()->SupportsEncodedOutput();
}

void MediaStreamRemoteVideoSource::RequestKeyFrame() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  if (!observer_ || !observer_->track()) {
    return;
  }
  scoped_refptr<webrtc::VideoTrackInterface> video_track(
      static_cast<webrtc::VideoTrackInterface*>(observer_->track().get()));
  if (video_track->GetSource()) {
    video_track->GetSource()->GenerateKeyFrame();
  }
}

base::WeakPtr<MediaStreamVideoSource>
MediaStreamRemoteVideoSource::GetWeakPtr() {
  return weak_factory_.GetWeakPtr();
}

bool MediaStreamRemoteVideoSource::AllowsVideoThreadTypeOverride() const {
  return true;
}

void MediaStreamRemoteVideoSource::SetHasSeenScreencastContentTypeCallback(
    base::OnceClosure callback) {
  PostCrossThreadTask(
      *video_task_runner(), FROM_HERE,
      CrossThreadBindOnce(
          &RemoteVideoSourceDelegate::SetHasSeenScreencastContentTypeCallback,
          delegate_, std::move(callback)));
}

void MediaStreamRemoteVideoSource::OnEncodedSinkEnabled() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  if (!observer_ || !observer_->track()) {
    return;
  }
  scoped_refptr<webrtc::VideoTrackInterface> video_track(
      static_cast<webrtc::VideoTrackInterface*>(observer_->track().get()));
  video_track->GetSource()->AddEncodedSink(delegate_.get());
}

void MediaStreamRemoteVideoSource::OnEncodedSinkDisabled() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  if (!observer_ || !observer_->track()) {
    return;
  }
  scoped_refptr<webrtc::VideoTrackInterface> video_track(
      static_cast<webrtc::VideoTrackInterface*>(observer_->track().get()));
  video_track->GetSource()->RemoveEncodedSink(delegate_.get());
}

}  // namespace blink
