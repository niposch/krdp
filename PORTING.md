# KDE upstream merge and local RDP integration — 2026-10-09

The maintained branch is `virtual-stock-kpipewire` in
`/home/niposch/Development/krdp-westers`. Official KDE KRDP `v6.7.5`
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

Exact runtime activation, binary hashes, Windows validation limits, backup and
rollback are recorded in
`/home/niposch/Administration/docs/krdp-color-codecs-and-merge.md`.
The prior 09:41 trial confirmed two Windows screens and pointer movement.
Post-merge Windows color/capture, local takeover and natural resume still need
checking. No beta Plasma/input dependency is adopted; KDE master additionally
requires libei >=1.6 and needs a separate future migration.

## Network estimate correction

FreeRDP 3.31.1 delivers bandwidth byte/time results to our callback without
populating `netCharBandwidth`. The inherited unsigned, uninitialized last-value
check could advertise arbitrary/zero bandwidth before a real sample. Compute
wire kilobits/s directly using 64-bit arithmetic, publish only after a valid
sample, and retain the connection's highest observed goodput as a lower-bound
estimate so an idle desktop does not lower the reported link estimate. This is
observed session throughput, not a capacity speed test. Adaptive quality still
uses the separate smoothed recent goodput. Scheduling runs on the peer thread
for both codecs; progressive frame sends no longer race/shorten these windows.
Missing-result recovery and 16-bit RTT sequence wrap are handled. Synthetic
protocol tests cover startup, FreeRDP's zero field, unit conversion, idle windows,
overflow and sequence wrap. Reconnect Windows to refresh its cached estimate.
