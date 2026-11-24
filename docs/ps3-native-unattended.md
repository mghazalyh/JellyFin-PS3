# Native reliability branch: unattended hardware diagnostics

The existing native JFR1 mailbox and Python FTP harness are integrated, with
no OpenCode orchestration dependency. This branch is not hardware validated.

- The client must be running an active Surf session. No XMB auto-launch,
  idle-screen remote control, arbitrary commands or arbitrary paths.
- Commands require a private 64-character lowercase hexadecimal key in
  /dev_hdd0/tmp/jf_remote/auth.key. The Python pair command provisions a key
  explicitly and refuses to overwrite it. FTP is not encrypted; use only the
  owner's trusted isolated LAN. Never commit the key or include it in logs.
- Missing, revoked, invalid or malformed keys cause commands to be discarded
  without publishing status. Deleting the key prevents future commands;
  an already accepted server action cannot necessarily be undone.
- A mutating command requires the active generation. Native dispatch is
  also bound to the Surf mode where the request was staged. Replay IDs are
  rejected within a fixed 16-entry window. One command is accepted at a time.
- Queries: get_status, capture_metrics. Controls: pause_surf, resume_surf,
  play_previous, select_previous, favorite_current, favorite_previous,
  select_current, set_audio_sync_beta, stop_playback.
- play_previous submits the latest eligible historical source and reports
  success only if Catalog confirms that source ID is playing. It is a shared
  source switch, not proof of the displayed picture. select_previous only
  selects history for a Favorite. Never retry a timed-out mutation blindly.
- Use tools/ps3_unattended.py probe, pair, send, collect, metrics, and soak
  from an existing Python 3 environment. The soak command is finite;
  by default it only observes. Optional control exercise changes the shared
  stream and must be used deliberately.
- The progress worker polls approximately once per second and writes a
  32-byte big-endian JFM1 resource sample about every five seconds, only during
  active Surf playback. File I/O is serialized; no disk or network work in
  renderer, decoder or PCM threads. Samples are separate from verbose logs.
- The current/previous JFM1 files are bounded to roughly 1 MiB each. A failed
  rotation does not extend the archive. Verbose log files have their own 2 MiB
  limits. The analyzer reports memory drift, not proof of a memory leak.
- Physical acceptance: pairing and revoked-key rejection, mode/session
  changes, bounded command errors, stable controller operation, confirmed
  Previous/Favorite/Pause/Resume, clean stop, no rising memory slope,
  repeated reconnects, actual A/V presentation, and long-duration Wi-Fi soak.
