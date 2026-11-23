# JellyFin PS3 Surf 3.6.4 beta (hardware audit)

Native PlayStation 3 Surf looping-channel support built using the pinned
ps3dev 2026-07-26 GCC 7.2 toolchain.

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

**Validation:** PS3 GCC 7.2.0 build completed, PKG payload verifier passed,
scene-cut logic test plus release-version, live-watch, Live TV and stream
request host tests passed. Existing macOS host toolchain cannot compile the
jitter-buffer test because its Linux malloc.h header is unavailable.

**Warning:** Version 3.6.3 Surf beta 1 is superseded and should not be used
for initial PS3 testing. Version 3.6.4 contains hardware-concurrency fixes.

**Beta limitation:** Frame/source attribution is heuristic because no source
epoch crosses Jellyfin's remux into video packets. Physical-console
synchronization and Favorite end-to-end behavior have not been verified yet.

**Install:** Transfer JellyFin-PS3.pkg to the PS3 Package Manager. Check the
SHA256 file if desired. For controls and logs, see docs/surf-ps3.md.

**Attribution:** Based on JellyFin [PS] for PS3 by jw-notes,
https://github.com/jw-notes/JellyFin-PS3. Existing credits and licensing
remain intact.
