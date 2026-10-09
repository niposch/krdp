# Building and checking the maintained fork

Maintained branch: `virtual-stock-kpipewire` at
[github.com/niposch/krdp](https://github.com/niposch/krdp). This is a GitHub fork
of KDE's mirror, with the official KDE v6.7.5 base and earlier westers custom
history merged without squashing or rewriting. KDE's source of truth is
[Invent](https://invent.kde.org/plasma/krdp). The fork's `master` branch retains the KDE upstream snapshot; it is not
this fork's maintained custom branch. Updating upstream remains an explicit merge.

## Dependencies and build

The validated stack on 2026-10-09 was Fedora 44 KDE with Qt >=6.10,
KDE Frameworks >=6.26, KPipeWire/KWin 6.7.5, FreeRDP/WinPR 3.31.1,
PipeWire 1.6.9 and WirePlumber 0.5.18. Install matching distribution development
packages for the CMake dependencies, including Qt private headers, ECM, KDE
Frameworks, Plasma Wayland protocols, QtKeychain, FreeRDP server/WinPR, KPipeWire,
xkbcommon and PAM. Tests need Qt Test and the KCM smoke test needs a session bus.
CMake reports any missing dependencies. No development Plasma upgrade is needed.

From a clean clone, choose a fresh build directory and private staging prefix:

```bash
git clone --branch virtual-stock-kpipewire https://github.com/niposch/krdp.git
cd krdp
cmake -S . -B build-niposch -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_INSTALL_PREFIX="$PWD/build-niposch/stage" \
  -DBUILD_PLASMA_SESSION=ON -DBUILD_TESTING=ON -DBUILD_EXAMPLES=ON \
  -DCMAKE_DISABLE_FIND_PACKAGE_Git=TRUE -DGIT_FOUND=FALSE \
  -DCMAKE_BUILD_RPATH_USE_ORIGIN=ON
cmake --build build-niposch --parallel 4
mkdir -p build-niposch/test-config
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
  XDG_CONFIG_HOME="$PWD/build-niposch/test-config" \
  ctest --test-dir build-niposch --timeout 25 --output-on-failure
```

Git discovery is disabled here solely to avoid CMake regenerating checkout hooks.
Appstream validation skips before installation; a KCM session-bus failure in a
sandbox is distinct from a failed codec test. Older inherited scripts and build
caches can name different private dependency paths or `/usr` as their install
prefix. Review their paths before use; the fresh build above is the current route.

## The separate KPipeWire color patch

KRDP source alone does not change RGB-to-YUV conversion inside KPipeWireRecord.
The exact patch is versioned at
[`patches/kpipewire-rdp-full-range-bt709.patch`](../patches/kpipewire-rdp-full-range-bt709.patch).
It was validated against official KPipeWire v6.7.5, base `6318082`, with matching
FFmpeg 8.1.2 runtime libraries and development headers. Full-range paths set
BT.709 matrix, primaries, transfer and encoded color metadata; Limited-range
recording remains unchanged. Older unrelated patches under `patches/kpipewire/`
are inherited experiments and are not prerequisites for this tested stack.

From the KRDP checkout, with matching KPipeWire/FFmpeg/VAAPI/GBM/DRM development
packages available:

```bash
git clone --branch v6.7.5 https://invent.kde.org/plasma/kpipewire.git ../kpipewire-color
git -C ../kpipewire-color apply --check "$PWD/patches/kpipewire-rdp-full-range-bt709.patch"
git -C ../kpipewire-color apply "$PWD/patches/kpipewire-rdp-full-range-bt709.patch"
cmake -S ../kpipewire-color -B ../kpipewire-color-build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF \
  -DCMAKE_SKIP_RPATH=ON \
  -DCMAKE_DISABLE_FIND_PACKAGE_Git=TRUE -DGIT_FOUND=FALSE
cmake --build ../kpipewire-color-build --parallel 4
```

For the validated private deployment, only `libKPipeWireRecord.so.6.7.5` and its
`.so.6` link accompany `krdpserver` and `libKRdp.so.6.7.5` in a private runtime.
Select that directory with `LD_LIBRARY_PATH` for the KRDP process. Keep system
KPipeWire, KPipeWireDmaBuf, KWin and user QML/plugin registrations untouched.
This private-library procedure assumes the matching 6.7.5 ABI; revalidate after
dependency updates. Review the launch paths and preserve the prior executable,
libraries and launch entries before deploying. A KRDP restart disconnects RDP
sessions; these instructions do not require restarting the desktop.

## Settings

For Windows multi-monitor, use `[General]` in `krdpserverrc`:

```ini
MonitorMode=virtual
VirtualMonitorLayout=client
VirtualMonitorPolicy=replace
Codec=auto
```

Run with `--plasma` for KWin virtual outputs. `replace` temporarily disables
physical outputs; use `extend` if the physical desktop must stay visible.
Keep the existing certificate/authentication configuration. The inherited
README below the fork summary explains output restore and local takeover.

Set `KRDP_BANDWIDTH_PROBE=1` in the server's environment to enable startup
bandwidth probing. It is disabled by default in source. The probe sends random
payloads before video, at most 1.3125 MiB total; its result estimates conditions
at connection time. Ongoing adaptive-quality goodput remains separate.
A reconnect with cached network characteristics cancels probing and uses the
client's supplied metrics. Remove the environment setting to use passive reporting.

Auto uses AVC420 when supported, otherwise progressive. `Codec=progressive`
or `KRDP_NO_H264=1` forces progressive on the next connection. Hardware H.264
remains the tested default. Stock KPipeWire has no dual-stream AVC444 encoder.
Image clipboard transfer is not implemented; text is supported in both directions.

## Additional reproducible checks

The CTest suite includes progressive round trips on non-64-pixel-aligned sizes,
per-monitor damage/queue/layout/input checks and tests against the production
network-detection implementation (units, packet limits, budgets, sequences,
zero/slow samples, cached reconnect, timeout and overflow).

Two portable helpers from the deployment investigation are included:

```bash
python3 scripts/check-peer-lifecycle.py "$PWD/build-niposch/bin/krdpserver"
python3 scripts/check-progressive-wire.py "$PWD/build-niposch" --xvfb /usr/bin/Xvfb
# Intel VAAPI AVC420 synthetic decode; needs FFmpeg and render-node permission:
python3 scripts/check-progressive-wire.py "$PWD/build-niposch" --xvfb /usr/bin/Xvfb --h264
```

Helpers need Python 3, OpenSSL and host process/network permissions. The wire
check also needs Pillow, Xvfb, ImageMagick `import` and `xfreerdp` on PATH.
Its `--h264` route uses `/dev/dri/renderD128` and assumes the tested Intel VAAPI
capabilities; adapt the render node for other hardware. It validates protocol,
codec and color on synthetic frames, rather than capturing the host desktop or
exercising the whole KPipeWire capture path. Both helpers isolate configuration,
use loopback ephemeral ports and temporary test certificates, and clean up their
processes/files. The wire fixture uses a disposable synthetic login.

## Verified result and remaining checks

On 2026-10-09 all 15 functional checks passed; appstream skipped before install.
Synthetic progressive and AVC420 two-surface decode passed. Twenty-five TLS
reconnects retained 10 file descriptors/two threads and shutdown completed within
eight seconds. Clipboard lazy-offer positive/negative checks preserved owned
Unicode text and read zero image/side payloads.

Windows RDP on the actual private deployment passed login, two monitors, pointer
movement, Spotify green and bandwidth display. Its corrected probe measured
262,400 bytes in 58 ms, or 36,193 kbit/s. The earlier probe's 16-KiB payloads
exceeded the 14-bit unfragmented PER length form; the corrected 8-KiB packets
passed Windows testing. Local takeover and another natural suspend/resume remain
follow-ups. The earlier missing-link issue was addressed by a separate stable
WirePlumber 0.5.18 update, not by a KRDP source patch. Bidirectional image
clipboard support remains a future feature requiring asynchronous bounded reads.
