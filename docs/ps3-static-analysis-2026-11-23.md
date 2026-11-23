# PS3 C/C++ static analysis baseline (2026-11-23)

Repository: mghazalyh/JellyFin-PS3
Audited revision: 3d45bc6 (Surf version 3.6.4)
Analyzer: Cppcheck 2.22.0, compiled from the official
https://github.com/cppcheck-opensource/cppcheck tag 2.22.0.

PS3 target: powerpc64-ps3-elf, PowerPC64 big endian, 8-byte PPU pointers.
Cppcheck uses unix64 type sizes; this does not model CellAudio, RSX, GPU or
decoder timing.

## Actual results

Surf-focused scan: 186 findings (90 warning, 96 style).
Broader scan: 222 selected C/C++ files, 406 findings (19 error,
343 warning, 42 portability, 2 performance).
Vendored implementation directories were excluded from the file list,
but included third-party headers may contribute diagnostics.

Of 406 diagnostics, 302 were uninitMemberVarNoCtor on aggregate
structures. These do not establish an actual uninitialized read;
many project structures are initialized at construction or call sites.

## Manually triaged findings

1. source/player/threads/player_threads.cpp:452:
   printf uses %u with s_au_submitted, declared signed int.
   This is a confirmed format mismatch, and should be fixed in a separate
   tested source change.
2. source/player/core/player_menu.cpp:101:
   items array warning; jf_audio_slot bounds the source tracks to
   JF_MAX_AUDIO and the loop fills the exact prefix passed to hud_open_menu.
   No unsafe access confirmed.
3. source/ui/xmb/ui_livetv.cpp:423, 992, 1004, 1028, 1249, 1357, 1577:
   error-class uninitialized-ps findings; progs_of populates pointers in
   [0,n) and each caller iterates [0,n). Likely analyzer false positives.
4. source/ui/xmb/ui_livetv.cpp:730:
   bounds warning; LP_GENRE_OF(key) < LG_COUNT guards GENRE_LABEL.
   Likely analyzer false positive.
5. source/audio/eac3/kbdwin.c:46:
   possible null int_window; float and fixed-point entry points pass the
   corresponding destination. Needs call-path validation if these change.
6. source/audio/adec_dts.cpp:156:
   possible uninitialized slice pointer. The channel loop fills [0,ch),
   but codec channel-count bounds should be verified separately.
7. source/audio/flac_dec.c:57:
   shift portability warning. The left-hand operand is uint32_t; conversion
   to signed after left shift is still implementation-dependent.

Some high-severity signed-shift warnings come from vendored FFmpeg
headers; they should be reviewed independently of our Surf features.

## Continuous automation

GitHub Actions workflow .github/workflows/ps3-cppcheck.yml scans
the source on push, pull request, and manual dispatch and uploads
ps3-cppcheck-report artifacts with full warning detail. This is
report-only until false-positive triage and baseline tracking improve.
The runner package version may differ from the 2.22.0 local scan.

Static analysis does not verify absence of memory leaks, races, real PS3
GPU hangs, or audio/video presentation synchronization. Continue using the
pinned PS3 GCC 7.2 compiler, sanitizer host tests, and hardware telemetry.
