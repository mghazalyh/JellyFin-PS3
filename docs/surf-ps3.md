# Surf controls in the native PlayStation 3 client

This fork extends JellyFin [PS] for the three custom Surf virtual Live TV
channels backed by CatalogService. Ordinary Live TV and movie playback use
their original control paths.

## Controller operation

While watching **Surf: All Channels**, **Surf: Sports** or **Surf: Movies & Series**:

1. Press **START** to open the built-in player Options menu.
2. Use the **D-pad** to move between Surf controls and **X** to choose an action.
3. **Pause Surf Looping** holds the current source. **Resume Surf Looping**
   continues automatic rotation.
4. **Favorite Current Channel** requests a Favorite for the currently published
   Surf source, not necessarily the frame still buffered by the decoder.
5. **Select Previous Channel** cycles the server's recent successfully played
   sources. **Favorite Selected Previous** uses the selected source and immutable
   32-character token; it does not change playback or silently select a new
   source after rotation.
6. **Select Current Channel** clears the Previous selection.
7. **Show Surf Source Status** displays the current publishing source.

A Favorites success message requires HTTP success from CatalogService, which
validates the selected source and writes to Jellyfin. If CatalogService cannot
resolve a unique original channel, it returns an error instead.

## Adaptive audio transition (beta)

On a source change, the PS3 keeps decoding audio but gates its PCM output. It
waits for a substantial decoded-picture change and for a queued flip to finish.
It then releases audio without an arbitrary fixed short delay. A 12-second
recovery bound prevents a permanent mute on a stalled source.

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

This prerelease's PKG is built with pinned ps3dev nightly-2026-07-26
GCC 7.2.0. Physical-console synchronization remains to be verified.
