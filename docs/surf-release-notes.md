# JellyFin PS3 Surf 3.6.3 beta

Native PlayStation 3 Surf looping-channel support built using the pinned
ps3dev 2026-07-26 GCC 7.2 toolchain.

**Included in the package**

* START Options menu via D-pad and X: pause/resume Surf, Favorite Current,
  select/cycle Previous, Favorite Selected Previous, select Current, status.
* Exact Previous source/token validation against authenticated CatalogService.
* Audio output gating on Surf source transitions until an observed decoded
  picture discontinuity has completed a PS3 screen flip.
* Bounded 12-second gate recovery, server/client correlation telemetry,
  source-epoch logs and diagnostic error handling.

**Validation:** PS3 GCC 7.2.0 build completed, PKG payload verifier passed,
scene-cut logic test plus release-version, live-watch, Live TV and stream
request host tests passed. Existing macOS host toolchain cannot compile the
jitter-buffer test because its Linux malloc.h header is unavailable.

**Beta limitation:** Frame/source attribution is heuristic because no source
epoch crosses Jellyfin's remux into video packets. Physical-console
synchronization and Favorite end-to-end behavior have not been verified yet.

**Install:** Transfer JellyFin-PS3.pkg to the PS3 Package Manager. Check the
SHA256 file if desired. For controls and logs, see docs/surf-ps3.md.

**Attribution:** Based on JellyFin [PS] for PS3 by jw-notes,
https://github.com/jw-notes/JellyFin-PS3. Existing credits and licensing
remain intact.
