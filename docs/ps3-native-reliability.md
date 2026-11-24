# Native Surf reliability engineering and validation

Base: hardware-readiness branch (20a55617). These changes target native PS3 and
do not alter Jellyfin's server-side shared MPEG-TS route, codecs or bitrate.

## Implemented in this branch

1. Watch distinct, successfully completed RSX picture flips instead of staged
   frames. Count failed flips separately; sequence zero and wrap are valid.
2. Detect first-picture stalls after a 15-second post-startup grace period.
   Retain the existing bounded, rate-limited Live TV reopen logic; leave
   ordinary Live TV behavior unchanged.
3. Emit sparse presentation heartbeat and stall events, with local jitter/TS
   ring and staged/confirmed-frame diagnostics. No per-frame HTTP.
4. Correlate source epochs, stream reconnections and controller actions using
   a playback trace prefix. Preserve opt-in PCM sync on same-mode reopen.
5. Play Previous via authenticated Catalog POST with exact historical source
   name, ID and token. Confirm server playing state, not merely HTTP acceptance.
   Poll at 1.5 seconds; each HTTP call has its own existing timeout.
6. Keep historical Favorite selection separate from shared stream switching.
   A successful switch clears the old selection; use Favorite Current.
7. Strictly parse complete top-level status and recent source objects; reject
   truncated IDs, non-hex tokens, nested identity decoys, and malformed JSON.
   No heap or large history arrays.
8. Keep the opt-in asynchronous player log bounded to approximately 2 MiB per
   file (current and previous). Batch flushes, count dropped messages, handle
   startup/write failures, and join the log thread on shutdown.
9. Host regressions for flips, startup stalls, reconnect limits, trace lifetime,
   history selection, escape decoding, rejected identity and status confirmation.
10. Cloud CI runs host regression, sanitizer and PowerPC64 endian comparison before the
    pinned PS3 SDK package build. No SDK installation on the Mac.
11. Port the existing opt-in JFR1 FTP mailbox and JFM1 metrics analyzer, without
    OpenCode tooling. Controls use the same native action worker. Bind commands
    to both generation and Surf mode; report success only after confirmation.
12. Serialize overlapping progress-worker file operations, recheck the key on
    every command, cap metrics rotation, and schedule samples without catch-up bursts.

## Next phases: not implemented or verified by this commit

| Workstream | Acceptance criteria |
| --- | --- |
| Media source epoch | Server-side TS marker survives Jellyfin remux and propagates through demux, decoder, flip and PCM. Do not infer epoch from scene cuts. |
| Audio clock | Compare actual presented video PTS and consumed audio PTS, handle wrap and discontinuities; hold new-generation audio only when supported by verified epochs. |
| Recovery | Distinguish network starvation, decoder progress, GPU flip stalls and upstream EOF; decoder-only restart only at safe IDR and parameter sets. Keep current reconnect fallback. |
| Buffering | Measure PCR reserve and underflows; tune bounded buffering without increasing Wi-Fi bitrate or decoded-frame memory blindly. |
| Resource ownership | Audit socket/decoder/PCM/thread cleanup and old-generation callbacks; test repeated stop/reopen cycles and memory slope on hardware. |
| Diagnostics | Add packet arrival, decoded-frame counters, actual presented PTS, consumed PCM and drift; validate log rotation and dropped counts physically. |
| Unattended hardware test | The bounded JFR1/JFM1 implementation is ported; provision a key explicitly on a trusted LAN, run real-console tests and verify clean stop, mode changes, response files, memory slope and log rotation. |
| Hardware validation | Test native PS3 Wi-Fi with startup, transitions, Previous/Favorites, long stalls, manual Stop, reconnects, controller responsiveness, A/V drift, and soak. |
| Server/client parity | Preserve authenticated shared transport; verify native route and exact source marker with production server. |

## Evidence and limitations

The audio gate remains a beta scene-cut heuristic with a finite mute ceiling;
server source identity is not proof of a presented native frame. Hardware tests
are required for timing, memory, PPU/SPU/RSX behavior, and actual Wi-Fi capacity.
The existing bounded reopen limit is deliberately preserved. No new package is
published as a production release by this branch's CI; installable PKG is a
GitHub Actions artifact after a successful build.
