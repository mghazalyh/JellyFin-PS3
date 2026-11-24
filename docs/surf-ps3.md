# Surf controls in the native PlayStation 3 client

This fork extends JellyFin [PS] for the three custom Surf virtual Live TV
channels backed by CatalogService. Ordinary Live TV and movie playback use
their original control paths.

## Controller operation

While watching **Surf: All Channels**, **Surf: Sports** or **Surf: Movies & Series**:

1. Press **START** to open the Surf Live TV menu during playback. Surf controls are first; general picture and statistics options follow. START or Circle closes the menu. It stays open while a channel action is pending.
2. Use the **D-pad** to move between Surf controls and **X** to choose an action.
3. **Pause Surf Looping** holds the current source. **Resume Surf Looping**
   continues automatic rotation.
4. **Favorite Current Channel** requests a Favorite for the currently published
   Surf source, not necessarily the frame still buffered by the decoder.
5. **Play Previous Channel** submits the exact historical source name, ID and
   immutable token to the authenticated Catalog Previous endpoint. A successful
   HTTP response is only acceptance; confirmation requires the server to report
   that exact source ID in playing state. This affects every viewer of that
   Surf mode. It does not prove the native decoder has presented the source.
6. **Select Current Channel** clears the Previous selection. After a confirmed
   switch use Favorite Current Channel; the old historical token is cleared.
7. **Show Surf Source Status** displays the current publishing source.

A Favorites success message requires HTTP success from CatalogService, which
validates the selected source and writes to Jellyfin. If CatalogService cannot
resolve a unique original channel, it returns an error instead.

## Wi-Fi Live TV stability policy

When the console reports a wireless connection, Live TV requests use a
stability-first 480p ceiling with a nominal 1.5 Mbps whole-transport
budget. The lower 360p choice remains available (700 kbps). Both budget
audio and TS overhead, and request transcoded audio instead of variable
rate HD audio copies. Stereo MP3 remains 192 kbps; requested AC-3 is
320 kbps. Wired Live TV and VOD request policies are unchanged. These
are conservative request targets, not measured guarantees of actual
server output or 802.11b throughput; verify on the intended console/AP.

## Adaptive audio transition (beta)

**The experimental adaptive sync starts OFF by default.** To test it,
choose START > Adaptive Audio Sync (Beta) > On while watching Surf.
A new playback resets it to Off.

When explicitly enabled, on a source change the PS3 keeps decoding audio
but gates its PCM output. It
waits for a substantial decoded-picture change and for a queued flip to finish.
It then releases audio without an arbitrary fixed short delay. A 3-second
recovery bound prevents an extended mute on a stalled source. The audio
writer alone owns live DMA ring writes; renderer code only sets a flag.

**Limitation:** There is no upstream-source epoch embedded in video packets
after Jellyfin remuxing. A luma scene change is a useful decoder-side
observation, not proof of an exact channel boundary; an unrelated scene cut
may trigger it, and transitions with similar frames may reach the bounded
timeout. Exact per-source presentation synchronization requires a robust
server-to-client epoch signal propagated through the actual decoder.

## Diagnostics

PS3 device log: /dev_hdd0/tmp/player_log.txt (or Settings > System >
Send Log to Server). Search for surf_sync: records. They include the
source epoch, privacy-preserving source hash, display sequence and PTS, cut
scores, time gated, and fallback reasons.

Authenticated Surf telemetry is sent to POST /api/surf/{mode}/telemetry
using source_changed, favorite_clicked, favorite_confirmed,
favorite_failed, previous_clicked, and playback_stalled. The trace
field correlates a client observation to the CatalogService log. Logs are
bounded by the server's standard rate limits and retention policy. No raw
stream URL or authentication token is logged.

Version 3.6.5 supersedes the earlier Surf betas. Its controller commands
avoid per-action heap allocations. Its PKG is built
with pinned ps3dev nightly-2026-07-26
GCC 7.2.0. Physical-console synchronization remains to be verified.
