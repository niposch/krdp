# KDE upstream compatibility candidate — 2026-10-09

This checkout was cloned directly from https://invent.kde.org/plasma/krdp.git.
Branch `port/6.7.5-custom` starts at official tag `v6.7.5`, commit
`270dcf01851229134f74fa7d90ac338ec788ec67`. The custom port is an uncommitted
patch on that base; it does not merge the westers fork's history into the branch.
Trial deployment now uses `/home/niposch/.local/libexec/krdp-upstream-6.7.5-20261009`.
The preserved `krdp-westers` checkout/runtime is the rollback installation.

## Retained behavior

- One capture/encoder/surface per monitor, client-shaped virtual outputs,
  replace/extend policy, layout ownership/reconciliation, physical-display
  snapshots/recovery, local takeover and the optional KRDPCTL channel.
- Monitor-aware input/pointer mapping and high-resolution/zero-position wheel
  handling; clipboard Unicode/newline conversion and echo guards.
- Text-only clipboard snapshots deployed October 8, H264 profile/VAAPI driver
  selection, AVC codec negotiation and optional adaptive quality.
- Existing custom settings and 12 pure-logic regression suites, plus upstream's
  settings-page smoke test; example servers/control probe build as well.

## Compatible upstream fixes retained or reapplied

- Official stable version/dependency declarations, translations, settings
  metadata and default logging severity. KF6 GlobalAccel and QtTest are added
  only for the retained shortcut/test functionality.
- `231bec4`: close the peer in every connection state before joining its thread.
- `125d8de`: free the peer context before freeing the peer, fixing connection
  file-descriptor/thread leaks.
- `9395b1c`: set KAboutData's correct desktop-file name (the fork already set the
  QApplication value, which KAboutData then overwrote).
- `5976316`: guard portal teardown when setup never created a session.
- Portal cursor metadata request; upstream private Wayland code generation.
- `4eb3dff` from KDE master: disable extended security for this candidate's
  existing TLS/PAM authentication path so FreeRDP 3.32 cannot bypass it.
- Clipboard MIME fix, fake-input authentication, main-thread input delivery,
  GFX capability re-advertisement handling and queue/surface locking already
  have equivalent custom implementations, which are preserved.

## Architectural difference that remains

This is a compatibility port, not a completed rewrite on upstream's new video
pipeline. Upstream 6.7 moved encoder ownership from AbstractSession to
VideoStream and added progressive encoding. The custom per-monitor sessions,
monitor-keyed frame queue, surfaces and display-readiness checks remain on the
custom H264 path. Consequently this candidate does not provide upstream's
progressive-codec fallback or every later VideoStream implementation change.
A genuine convergence must move those custom features onto upstream's encoder
ownership/surface interfaces and rerun authenticated Windows two-monitor,
clipboard, local takeover and suspend/resume checks. Current master (6.8.80)
also changes input to libei and requires libei >=1.6; libei-devel is not installed
on this workstation. Plasma 6.8 is still a beta as of this investigation.

## Verification and deployment status

Full RelWithDebInfo build, all examples and settings module succeeded against
stock Fedora KPipeWire 6.7.5, Qt 6.11.2, KF6 6.30 and FreeRDP 3.31.1.
All 14 CTest entries pass with host D-Bus, offscreen/software rendering and
isolated settings; appstreamtest skips because no install was performed, so
13 entries execute functional checks. Both clipboard snapshot handlers pass
image-only/mixed-offer regression checks with the old fork HEAD as the negative
control. Twenty-five loopback-only TLS connections leave FD/thread counts at
10/2 before and after; termination succeeds within eight seconds (exit 0).
No credentials are sent and no authenticated capture session is exercised.
The first sandboxed KCM test hung; the isolated host-side run passes. Test
processes were cleaned up. Trial activation on October 9 at 09:41:23 CEST replaced the live server.
The user confirmed both Windows RDP screens and pointer crossing work.
Capture at 1920x1080 and 3440x1440 runs on automatic error-free PipeWire links.
Repeated real screenshot/text copies, local takeover and natural resume remain
to be checked; this does not complete integration of upstream's encoder pipeline.

Reproduction, dependency transaction, staged artifacts and the remaining
activation/convergence work are recorded in
`/home/niposch/Administration/docs/krdp-upstream-migration.md` and
`/home/niposch/Administration/scripts/krdp-upstream-port/README.md`.
