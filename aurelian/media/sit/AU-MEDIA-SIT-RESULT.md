# AU-MEDIA-SIT — live-binary confirmation of the avatar virtual camera

Closes the deferred **live-binary** leg of AU-MEDIA-SIT (DEFERRAL-LEDGER /
DEFERRAL-INTERROGATION #7). The capability was already proven in-process by
`media/aurelian_virtual_camera_browsertest.cc`; this is the higher-fidelity
confirmation that the **boot-wired** avatar virtual camera is real in the
actual built `Chromium.app`, enumerable to the web, and delivers live frames
through the **real** (out-of-process) video-capture service.

## What the SIT does

`live_media_sit.py` launches the built fork binary headless with
`--remote-debugging-port`, drives it over CDP (no puppeteer/playwright — none
of the Legion browser handles wrap the *fork* binary, so the fork must be
launched directly), and on a `file://` page:

1. `enumerateDevices()` — finds the `videoinput` labelled **"Aurelian Virtual
   Camera"** (the boot-registered device; `kDeviceId = "/aurelian/camera"`),
2. `getUserMedia({video:{deviceId:{exact}}})` — opens it through the real
   capture service,
3. attaches it to a `<video>`, draws a frame to a canvas, samples mean luma,
4. captures a full-page screenshot.

Pass iff the camera is enumerated **and** gUM opens **and** a non-black frame
paints. Exit 0 = GREEN, 1 = RED.

## The macOS GpuMemoryBuffer discriminator (RED-first)

On macOS the video-capture service defaults to GpuMemoryBuffer-backed frames,
which the virtual device's buffer path cannot satisfy:

```
FATAL:services/video_capture/broadcasting_receiver.cc:154]
  NOTREACHED hit. Unexpected GpuMemoryBuffer handle type
  ...
  video_capture::BroadcastingReceiver::BufferContext::CloneBufferHandle(...)
  video_capture::BroadcastingReceiver::AddClient(...)
  video_capture::PushVideoStreamSubscriptionImpl::Activate()
```

The documented launch workaround is `--disable-video-capture-use-gpu-memory-buffer`.
The SIT applies it by default (correct production launch); `--demo-red` omits it
to reproduce the failure. This is the RED→GREEN discriminator on **observed**
behaviour — not a returned-but-unasserted value.

## Run of record — 2026-06-05, fork @ 87266dd, Chromium 148.0.7765.0 (arm64, macOS 25.2.0)

| mode | flag | camFound | gumOk | frameOk | meanLuma | canvas | exit |
|------|------|----------|-------|---------|----------|--------|------|
| **GREEN** (default) | `--disable-video-capture-use-gpu-memory-buffer` applied | ✅ | ✅ | ✅ | **130** | uniform gray | **0** |
| **RED** (`--demo-red`) | workaround omitted | ✅ | ✅ | ❌ | **0** | fully black | **1** |

`enumerateDevices` videoinputs observed: `Aurelian Virtual Camera`, plus the
Asmodeus virtual cameras. `meanLuma ≈ 130` is the boot-default avatar frame
(MediaSeam Y/U/V = 128 until the agent's avatar feeds pixels) — i.e. a genuine
live frame from the Aurelian device, not a fake-device beep-ball (no
`--use-fake-device-for-media-stream` is passed; only `--use-fake-ui-...` to
auto-grant the permission). Screenshots were Read-verified: GREEN = gray frame
painted; RED = black (no frame reached the page). The RED FATAL above is the
exact NOTREACHE the deferral named.

## How to run

```
python3 aurelian/media/sit/live_media_sit.py          # GREEN (correct launch)
python3 aurelian/media/sit/live_media_sit.py --demo-red # reproduce the NOTREACHE
# AURELIAN_CHROME=... overrides the binary path; AURELIAN_SIT_WORKDIR=... the scratch dir.
```
