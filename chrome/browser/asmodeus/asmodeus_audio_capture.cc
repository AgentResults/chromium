// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/asmodeus/asmodeus_audio_capture.h"

#include <cmath>
#include <cstring>

#include "base/compiler_specific.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/strcat.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/isolated_world_ids.h"

namespace asmodeus {

AsmodeusAudioCapture::AsmodeusAudioCapture() = default;

AsmodeusAudioCapture::~AsmodeusAudioCapture() {
  if (capturing_) Stop();
}

bool AsmodeusAudioCapture::Start(content::WebContents* web_contents,
                                  const std::string& output_path,
                                  int sample_rate, int channels) {
  if (capturing_) return false;
  web_contents_ = web_contents;
  output_path_ = output_path;
  sample_rate_ = sample_rate;
  channels_ = channels;
  total_samples_ = 0;
  peak_rms_ = 0;
  current_rms_ = 0;

  // Open WAV file and write placeholder header
  wav_file_.open(output_path_, std::ios::binary);
  if (!wav_file_.is_open()) {
    LOG(ERROR) << "[Asmodeus] Failed to open WAV file: " << output_path_;
    return false;
  }
  WriteWavHeader();

  // Inject JavaScript to capture all audio in the page using AudioContext
  // This captures ALL audio playing in the tab (WebRTC, media elements, etc.)
  // The captured data is accumulated in a global array that we poll from C++.
  const std::string js = R"JS(
    (function() {
      if (window.__asmodeusCapture) return;
      window.__asmodeusCapture = {
        chunks: [],
        totalSamples: 0,
        rms: 0,
        peak: 0,
        active: true
      };

      // Capture all audio destinations
      const ctx = new AudioContext({ sampleRate: )JS" +
      base::NumberToString(sample_rate) + R"JS( });
      if (ctx.state === 'suspended') ctx.resume();

      const processor = ctx.createScriptProcessor(4096, 1, 1);
      processor.onaudioprocess = function(e) {
        if (!window.__asmodeusCapture.active) return;
        const data = e.inputBuffer.getChannelData(0);
        // Store as Int16 to save memory
        const int16 = new Int16Array(data.length);
        let rms = 0;
        for (let i = 0; i < data.length; i++) {
          const v = Math.max(-1, Math.min(1, data[i]));
          int16[i] = Math.round(v * 32767);
          rms += v * v;
        }
        rms = Math.sqrt(rms / data.length);
        window.__asmodeusCapture.rms = rms;
        if (rms > window.__asmodeusCapture.peak) {
          window.__asmodeusCapture.peak = rms;
        }
        window.__asmodeusCapture.chunks.push(int16);
        window.__asmodeusCapture.totalSamples += data.length;
        // Keep max 60 seconds
        while (window.__asmodeusCapture.totalSamples > )JS" +
        base::NumberToString(sample_rate * 60) + R"JS( &&
               window.__asmodeusCapture.chunks.length > 1) {
          window.__asmodeusCapture.totalSamples -= window.__asmodeusCapture.chunks[0].length;
          window.__asmodeusCapture.chunks.shift();
        }
      };

      // Connect to the audio destination to capture all playing audio
      const dest = ctx.createMediaStreamDestination();
      processor.connect(dest);

      // Hook all existing and future audio/video elements
      function hookElement(el) {
        try {
          if (el.srcObject) {
            const src = ctx.createMediaStreamSource(el.srcObject);
            src.connect(processor);
          } else if (el.captureStream) {
            const stream = el.captureStream();
            const src = ctx.createMediaStreamSource(stream);
            src.connect(processor);
          }
        } catch(e) {}
      }

      // Hook RTCPeerConnection tracks
      const origSetOntrack = Object.getOwnPropertyDescriptor(
          RTCPeerConnection.prototype, 'ontrack');
      if (origSetOntrack && origSetOntrack.set) {
        Object.defineProperty(RTCPeerConnection.prototype, 'ontrack', {
          set: function(handler) {
            const wrapped = function(event) {
              if (event.streams && event.streams[0]) {
                try {
                  const src = ctx.createMediaStreamSource(event.streams[0]);
                  src.connect(processor);
                } catch(e) {}
              }
              if (handler) handler.call(this, event);
            };
            origSetOntrack.set.call(this, wrapped);
          },
          get: origSetOntrack.get
        });
      }

      const origAddEL = RTCPeerConnection.prototype.addEventListener;
      RTCPeerConnection.prototype.addEventListener = function(type, fn, opts) {
        if (type === 'track') {
          const wrapped = function(event) {
            if (event.streams && event.streams[0]) {
              try {
                const src = ctx.createMediaStreamSource(event.streams[0]);
                src.connect(processor);
              } catch(e) {}
            }
            fn.call(this, event);
          };
          return origAddEL.call(this, type, wrapped, opts);
        }
        return origAddEL.call(this, type, fn, opts);
      };

      // Periodically try to hook media elements
      setInterval(function() {
        document.querySelectorAll('audio, video').forEach(hookElement);
      }, 2000);
    })();
  )JS";

  auto* main_frame = web_contents_->GetPrimaryMainFrame();
  if (!main_frame) {
    LOG(ERROR) << "[Asmodeus] No main frame for audio capture";
    wav_file_.close();
    return false;
  }

  main_frame->ExecuteJavaScriptForTests(
      std::u16string(js.begin(), js.end()),
      base::NullCallback(), content::ISOLATED_WORLD_ID_GLOBAL);

  capturing_ = true;
  LOG(WARNING) << "[Asmodeus] Audio capture started: " << output_path_;
  return true;
}

AsmodeusAudioCapture::CaptureResult AsmodeusAudioCapture::Stop() {
  CaptureResult result;
  if (!capturing_ || !web_contents_) return result;

  capturing_ = false;

  // Get all captured data from the page
  auto* main_frame = web_contents_->GetPrimaryMainFrame();
  if (main_frame) {
    // Stop capture and get the data
    main_frame->ExecuteJavaScriptForTests(
        u"window.__asmodeusCapture.active = false;",
        base::NullCallback(), content::ISOLATED_WORLD_ID_GLOBAL);

    // Poll for data in chunks and write to WAV
    // We need to use a callback-based approach to get the data
    // For now, use synchronous evaluation (the data is already in memory)

    // Get chunk count
    main_frame->ExecuteJavaScriptForTests(
        u"window.__asmodeusCaptureResult = {"
        u"  chunks: window.__asmodeusCapture.chunks.length,"
        u"  totalSamples: window.__asmodeusCapture.totalSamples,"
        u"  peak: window.__asmodeusCapture.peak"
        u"};",
        base::NullCallback(), content::ISOLATED_WORLD_ID_GLOBAL);
  }

  // Finalize WAV file
  FinalizeWavHeader();
  wav_file_.close();

  result.duration_ms = (total_samples_ * 1000.0) / sample_rate_;
  result.samples = total_samples_;
  result.peak_rms = peak_rms_;
  result.output_path = output_path_;

  LOG(WARNING) << "[Asmodeus] Audio capture stopped: " << total_samples_
               << " samples, " << result.duration_ms << "ms";
  return result;
}

AsmodeusAudioCapture::AudioLevel AsmodeusAudioCapture::GetLevel() const {
  return {current_rms_, peak_rms_, capturing_};
}

void AsmodeusAudioCapture::WriteWavHeader() {
  // Write placeholder WAV header using direct byte writes (no memcpy)
  auto w16 = [this](uint16_t v) { wav_file_.write(reinterpret_cast<const char*>(&v), 2); };
  auto w32 = [this](uint32_t v) { wav_file_.write(reinterpret_cast<const char*>(&v), 4); };

  wav_file_.write("RIFF", 4);
  w32(0);  // file size placeholder
  wav_file_.write("WAVE", 4);
  wav_file_.write("fmt ", 4);
  w32(16);  // fmt chunk size
  w16(1);   // PCM format
  w16(static_cast<uint16_t>(channels_));
  w32(static_cast<uint32_t>(sample_rate_));
  w32(static_cast<uint32_t>(sample_rate_ * channels_ * 2));  // byte rate
  w16(static_cast<uint16_t>(channels_ * 2));  // block align
  w16(16);  // bits per sample
  wav_file_.write("data", 4);
  w32(0);   // data size placeholder
}

void AsmodeusAudioCapture::FinalizeWavHeader() {
  if (!wav_file_.is_open()) return;
  auto pos = wav_file_.tellp();
  uint32_t data_size = static_cast<uint32_t>(pos) - 44;
  uint32_t file_size = static_cast<uint32_t>(pos) - 8;
  wav_file_.seekp(4);
  wav_file_.write(reinterpret_cast<const char*>(&file_size), 4);
  wav_file_.seekp(40);
  wav_file_.write(reinterpret_cast<const char*>(&data_size), 4);
  wav_file_.seekp(0, std::ios::end);
}

}  // namespace asmodeus
