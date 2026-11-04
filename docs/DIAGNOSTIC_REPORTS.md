# Diagnostic reports

The launcher already provides **Troubleshooting > Create diagnostic report**.
It saves a local ZIP; nothing is uploaded automatically. Reports exclude game
data, saves, prepared images, GPU command dumps and screenshots.

## Current source changes after 0.5.4

- New launcher sessions enable the existing `--frame-stats` counters. Every five
  seconds the runtime reports guest-submitted FPS and frame-time percentiles.
  This does not change the selected frame rate or enable generated frames.
- Each session records the selected frame rate, fullscreen and widescreen flags,
  alongside the existing window, input, audio and online settings. The report's
  current settings may differ from an older run: inspect that run's session.json.
- Reports keep the first 256 KiB of game.log as startup.log, in addition to the
  last 8 MiB. This preserves early device/driver diagnostics when verbose logs
  push them out of the tail. Files shorter than the limit are kept in full.
- Known profile/name values, executable/data/game roots, configured player name
  and peer endpoints are redacted, including JSON-escaped and alternate-slash
  paths. Longest matches take priority. Short names match whole words.
- Session directories inside the ZIP use numeric indexes instead of source
  folder names. Review the report before sharing: redaction of known values is
  not a guarantee that arbitrary third-party log text contains no identifiers.

The existing ZIP writer is retained. Input logs stay unchanged. A report from
an older session cannot retroactively acquire timing samples that were not logged.

## Reading performance evidence

Window dimensions describe presentation, not internal render resolution. An
inspection of ten game-only PNG headers from the 0.5.4 widescreen test found
1280x720; ten corresponding window captures were 2560x1080. This verifies output
buffer dimensions, not the dimensions of every intermediate render pass. FSR 4
and FSR 3 frame generation remain unimplemented.

Selected 60 FPS is a request; check the runtime's patch result and actual frame
timing before concluding it was applied or sustained. Startup/loading stalls,
minimized periods, and gameplay should be assessed separately. These counters
measure submitted frames, not independently verified display refreshes, input
latency or gameplay speed.

## Validation

Windows launcher-core tests cover escaped paths, path separators, overlapping
identities, short names, preserved timing values, settings without private fields,
and bounded head/tail reads. JSON parsing and ZIP CRC checks independently verify
the generated synthetic fixtures. A separate Windows launcher executable was
compiled and linked; no installed game files were replaced. Interactive report
dialog testing remains pending while a player session is active.
