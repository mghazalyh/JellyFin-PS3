# JellyFin PS3 Surf 3.6.5 beta (GitHub-audited build)

Native PlayStation 3 Surf looping-channel support built using the pinned
ps3dev 2026-07-26 GCC 7.2 toolchain.

**Fixes since 3.6.4**

* Surf controller network commands now reuse a fixed-size command buffer
  without allocating or freeing a request and auth token on each click.
* Added DTS-HD channel/plane bounds checks and portable FLAC signed residual
  conversion with extensive host tests.
* Corrected telemetry format type, redundant counter tracking, cancelled
  Live TV search input initialization, and NUL literals in XMB source.
* GitHub cloud tests and pinned PS3 GCC 7.2 cross-builds generate the package
  entirely on disposable GitHub runners, not the developer's Mac Mini.

**Included in the package**

* START Options menu via D-pad and X: pause/resume Surf, Favorite Current,
  select/cycle Previous, Favorite Selected Previous, select Current, status.
* Exact Previous source/token validation against authenticated CatalogService.
* Experimental adaptive PCM audio gate, OFF by default until physical console
  testing; opt in through START > Adaptive Audio Sync (Beta).
* Audio-ring clearing owned only by the audio writer, display-frame sequence
  captured before upload acknowledgement and correlated with a completed flip.
* Bounded 3-second gate recovery, server/client correlation telemetry,
  source-epoch logs and diagnostic error handling.
* Favorite commands now precede telemetry, and status checks take priority
  over remote telemetry. Stale controller actions are isolated by generation.

**Validation:** GitHub Actions requires the full host regression suite, PS3
GCC 7.2 cross-build and package payload verification before release.
Static-analysis results are saved in separate Actions artifacts.

**Warning:** Versions 3.6.3 and 3.6.4 are superseded by 3.6.5.
Physical PS3 playback and exact A/V source synchronization are unverified.

**Beta limitation:** Frame/source attribution is heuristic because no source
epoch crosses Jellyfin's remux into video packets. Physical-console
synchronization and Favorite end-to-end behavior have not been verified yet.

**Install:** Transfer JellyFin-PS3.pkg to the PS3 Package Manager. Check the
SHA256 file if desired. For controls and logs, see docs/surf-ps3.md.

**Attribution:** Based on JellyFin [PS] for PS3 by jw-notes,
https://github.com/jw-notes/JellyFin-PS3. Existing credits and licensing
remain intact.
