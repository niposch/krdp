# KDE upstream merge and local RDP integration — 2026-10-09

The maintained branch is `virtual-stock-kpipewire` in
[niposch/krdp](https://github.com/niposch/krdp), a GitHub fork of KDE/krdp. Official KDE KRDP `v6.7.5`
(`270dcf01851229134f74fa7d90ac338ec788ec67`) is merged into our existing history.
The temporary KDE-based `port/6.7.5-custom` branch was the tested integration
vehicle; it does not replace the original maintained branch.

## Preserved custom behavior

- One capture/encoder/surface per monitor, client-sized virtual outputs,
  replace/extend policy, physical-output snapshots and restore, local takeover,
  layout ownership/reconciliation and optional KRDPCTL control.
- Existing monitor-aware pointer mapping, cursor metadata, wheel handling,
  stream recovery, VAAPI/profile choice and optional adaptive quality.
- Clipboard Unicode/newline handling, ownership, main-thread delivery,
  client-echo and multi-monitor announcement guards. Text snapshots avoid the
  eager image reads that caused the October 8 freeze.
- Custom settings, examples, layout/input/recovery tests and fork documentation.

## Upstream integration

Stable version/dependency declarations, translations, settings metadata,
peer shutdown/context cleanup, portal app-ID/teardown/cursor fixes and Wayland
private code generation are retained. Compatible KDE master `4eb3dff` disables
extended security for the existing TLS/PAM authentication path with FreeRDP 3.32.
The stable desktop and FreeRDP 3.31.1 packages remain in use.

Upstream encoder ownership and progressive encoding are adapted to multiple
monitors through `CaptureStream`: server capture pipelines belong to
`VideoStream`, while session backends retain their capture/recovery/input
facade. Standalone capture examples own their own pipeline. Encoders wait for
codec negotiation before starting. Each progressive surface has independent
codec state, full initialization after reset, monitor-specific queued damage
coalescing and bounded in-flight frames. H.264 keeps ordered P-frame chains and
monitor-specific keyframe clearing. Caps re-advertisement keeps our surface
locking and recovery behavior.

This is an integration of upstream behavior, not an identical copy of its
single-monitor VideoStream. Auto uses H.264 AVC420 when available and
progressive otherwise. `Codec=progressive` or `KRDP_NO_H264=1` forces progressive.
Stock KPipeWire has no dual-stream AVC444 encoder: misleading AVC444 advertising
and logging are corrected. The existing AVC444 preference is retained but
falls back to AVC420 with this encoder; capability helpers remain for a future
true AVC444 implementation.

## Color and clipboard findings

The Spotify Flatpak icon uses RGB (30,215,96). The Intel VAAPI default conversion
reproduces a reduction to (28,180,97); explicit BT.709 matrix/primaries/transfer
and full-range encoding recover approximately (31,215,98). The private
KPipeWireRecord patch under `patches/` corrects the full-range hardware and
software encoder paths, including encoded color metadata. Its Limited recording
path remains unchanged. Build the patch against official KPipeWire v6.7.5;
only KRDP's private runtime should load this library, preserving desktop packages.

Text copying both ways was confirmed by the user. Images are unimplemented
in our branch and upstream, rather than a failed redirection setting. Adding
PNG/CF_DIB transfer requires deferred requested-format reads, bounded conversion,
ownership/generation handling and image echo guards. Do not fetch every advertised
Wayland payload from a clipboard-change handler.

## Validation and operation

All 16 CTest entries pass (15 functional checks; appstream skips before install),
including non-tile-aligned progressive codec round trips and monitor-specific
damage preservation. Clipboard snapshot negative/positive regression checks pass.
A synthetic loopback RDP server and FreeRDP client on an isolated X display decode
both monitor surfaces correctly through progressive and Intel-encoded AVC420;
progressive also initializes both surfaces from partial first-frame damage.
These tests never capture the desktop or use system login/clipboard data.

Portable build, dependency-patch and validation instructions are in
[docs/fork-build.md](docs/fork-build.md). Windows RDP on the final corrected
merged runtime confirmed both screens, pointer movement, Spotify green and
bandwidth display; its actual startup probe measured 36,193 kbit/s.
Local takeover and another natural resume still need checking. No beta Plasma/input dependency is adopted; KDE master additionally
requires libei >=1.6 and needs a separate future migration.

## Network estimate correction and active probe

FreeRDP 3.31.1 delivers bandwidth byte/time results to our callback without
populating `netCharBandwidth`. The inherited unsigned, uninitialized last-value
check could advertise arbitrary/zero bandwidth before a real sample. Compute
wire kilobits/s directly using 64-bit arithmetic and publish only valid results.

An experimental connection-time protocol probe measures bandwidth independently of screen
activity. After an RTT request, send random RDP bandwidth payloads: 64 KiB first,
then 256 KiB and at most 1 MiB only when the prior round takes under 20 ms.
Total payload is capped at 1.3125 MiB; no probe traffic continues during ordinary
streaming. An eight-second handshake deadline closes an unresponsive peer;
The probe is disabled by default: `KRDP_BANDWIDTH_PROBE=1` explicitly enables it.
The first Windows trial failed with 0x1304 and was rolled back. The corrected
8-KiB payload stays within PER's 14-bit unfragmented length form; Windows
now completes real probe exchanges (~54–87 Mbit/s in the synthetic LAN test).
The synthetic login then closed; the user reported no test password entry,
consistent with a fixture authentication failure. NETCHAR_SYNC
now cancels probing and uses cached metrics as required by MS-RDPBCGR; this
regression is tested. Normal-server login/display validation subsequently passed with Windows RDP.
Clients without
negotiated auto-detection skip it. Probe results are an estimate at connection
start, not a guarantee of Wi-Fi/link capacity for the entire session. Reconnect
to remeasure. The FreeRDP client wire test verifies actual payload/results and
both monitor surfaces, not just simulated arithmetic.

Passive goodput remains separate for adaptive quality. A successful active probe
sets the client-visible estimate; idle traffic cannot replace it with application
demand. If the probe yields no usable result, retain the highest observed session
goodput as a fallback lower bound. All continuous measurement scheduling runs on
the peer thread, including progressive. Missing-result recovery and 16-bit RTT
sequence wrap are handled. Regression tests cover startup, units, idle demand,
byte limits, slow/zero-ms samples, sequences, deadline and overflow.
