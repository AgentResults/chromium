#!/usr/bin/env python3
"""AU-MEDIA-SIT: live-binary confirmation of the Aurelian avatar virtual camera.

Launches the built fork Chromium.app, drives getUserMedia over CDP against the
boot-wired avatar virtual camera, and asserts OBSERVED behaviour:
  1. "Aurelian Virtual Camera" is enumerable in the real binary,
  2. getUserMedia opens it through the REAL video-capture service,
  3. it delivers a valid live frame (non-black) that paints to a canvas,
  4. a full-page screenshot is captured for visual verification.

The macOS GpuMemoryBuffer NOTREACHE is the RED/GREEN discriminator:
  --gmb-off  -> pass --disable-video-capture-use-gpu-memory-buffer  (GREEN)
  (omit it)  -> capture service fails to bind a context provider     (RED)
"""
import argparse, json, os, shutil, subprocess, sys, time, urllib.request
import websocket  # websocket-client

CHROME = os.environ.get(
    "AURELIAN_CHROME",
    "/Users/admin/workspace/chromium/src/out/Default/"
    "Chromium.app/Contents/MacOS/Chromium")
CAM_LABEL = "Aurelian Virtual Camera"
WORKDIR = os.environ.get("AURELIAN_SIT_WORKDIR", "/tmp/au-sit")

PAGE = """
<!doctype html><html><body style="margin:0;background:#000">
<video id=v autoplay playsinline muted style="display:none"></video>
<canvas id=c width=320 height=240 style="width:100vw;height:100vh"></canvas>
<script>
window.SIT = {phase:'start', devices:[], camFound:false, gumOk:false,
             frameOk:false, meanLuma:-1, err:null};
async function run(){
  try{
    const all = await navigator.mediaDevices.enumerateDevices();
    SIT.devices = all.filter(d=>d.kind==='videoinput').map(d=>d.label);
    const cam = all.find(d=>d.kind==='videoinput' && d.label==='%CAM%');
    SIT.camFound = !!cam;
    if(!cam){ SIT.phase='no-camera'; return; }
    const stream = await navigator.mediaDevices.getUserMedia(
      {video:{deviceId:{exact:cam.deviceId}}});
    SIT.gumOk = true;
    const v=document.getElementById('v'); v.srcObject=stream;
    await v.play();
    // Wait for a real painted frame.
    for(let i=0;i<60 && (v.videoWidth===0);i++){ await new Promise(r=>setTimeout(r,100)); }
    const c=document.getElementById('c'), g=c.getContext('2d');
    await new Promise(r=>setTimeout(r,300));
    g.drawImage(v,0,0,c.width,c.height);
    const px=g.getImageData(0,0,c.width,c.height).data;
    let sum=0,n=0;
    for(let i=0;i<px.length;i+=4){ sum += (px[i]+px[i+1]+px[i+2])/3; n++; }
    SIT.meanLuma = sum/n;
    SIT.frameOk = SIT.meanLuma > 4;   // not all-black
    SIT.phase='done';
  }catch(e){ SIT.err = String(e); SIT.phase='error'; }
}
run();
</script></body></html>
""".replace("%CAM%", CAM_LABEL)


def cdp(ws, mid, method, params=None):
    ws.send(json.dumps({"id": mid, "method": method, "params": params or {}}))
    while True:
        msg = json.loads(ws.recv())
        if msg.get("id") == mid:
            if "error" in msg:
                raise RuntimeError(f"{method}: {msg['error']}")
            return msg.get("result", {})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--demo-red", action="store_true",
                    help="OMIT the macOS GMB workaround flag to reproduce the "
                         "documented NOTREACHE (capture service dies, zero "
                         "frames, black canvas) — the RED half of the SIT")
    ap.add_argument("--port", type=int, default=9333)
    ap.add_argument("--shot", default=os.path.join(WORKDIR, "live_media.png"))
    args = ap.parse_args()
    # The correct macOS production launch applies the workaround by default.
    apply_gmb_workaround = not args.demo_red

    os.makedirs(WORKDIR, exist_ok=True)
    profile = os.path.join(WORKDIR, "profile")
    shutil.rmtree(profile, ignore_errors=True)
    pagefile = os.path.join(WORKDIR, "media.html")
    with open(pagefile, "w") as f:
        f.write(PAGE)

    flags = [
        CHROME, "--headless=new", "--no-sandbox", "--disable-gpu",
        f"--remote-debugging-port={args.port}",
        "--remote-allow-origins=*",
        f"--user-data-dir={profile}",
        "--use-fake-ui-for-media-stream",      # auto-grant cam/mic, REAL devices
        "--window-size=320,240",
        "--no-first-run", "--no-default-browser-check",
    ]
    if apply_gmb_workaround:
        flags.append("--disable-video-capture-use-gpu-memory-buffer")
    flags.append("about:blank")

    log = open(os.path.join(WORKDIR, "cdp_launch.log"), "w")
    proc = subprocess.Popen(flags, stdout=log, stderr=subprocess.STDOUT)
    try:
        # Wait for the DevTools endpoint.
        page_ws = None
        for _ in range(100):
            try:
                data = json.load(urllib.request.urlopen(
                    f"http://localhost:{args.port}/json", timeout=1))
                for t in data:
                    if t.get("type") == "page" and t.get("webSocketDebuggerUrl"):
                        page_ws = t["webSocketDebuggerUrl"]; break
                if page_ws:
                    break
            except Exception:
                pass
            time.sleep(0.2)
        if not page_ws:
            print("FAIL: no DevTools page target"); return 2

        ws = websocket.create_connection(page_ws, timeout=30)
        mid = [0]
        def call(m, p=None):
            mid[0] += 1; return cdp(ws, mid[0], m, p)
        call("Page.enable"); call("Runtime.enable")
        url = "file://" + pagefile
        call("Page.navigate", {"url": url})

        # Poll SIT.phase until terminal.
        result = {}
        for _ in range(120):
            r = call("Runtime.evaluate",
                     {"expression": "JSON.stringify(window.SIT||{})",
                      "returnByValue": True})
            val = r.get("result", {}).get("value")
            if val:
                result = json.loads(val)
                if result.get("phase") in ("done", "error", "no-camera"):
                    break
            time.sleep(0.25)

        shot = call("Page.captureScreenshot", {"format": "png"})
        import base64
        with open(args.shot, "wb") as f:
            f.write(base64.b64decode(shot["data"]))

        print("SIT RESULT:", json.dumps(result, indent=2))
        print("screenshot:", args.shot)
        ws.close()

        ok = (result.get("camFound") and result.get("gumOk")
              and result.get("frameOk"))
        if ok:
            print(f"GREEN: avatar camera live; meanLuma={result.get('meanLuma'):.1f}")
            return 0
        print(f"RED: camFound={result.get('camFound')} "
              f"gumOk={result.get('gumOk')} frameOk={result.get('frameOk')} "
              f"err={result.get('err')}")
        return 1
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()


if __name__ == "__main__":
    sys.exit(main())
