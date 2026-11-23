# PS3 unattended harness: engineering contract and test procedure

Status: development branch only. This is not a 3.6.6 release and is not
hardware validated. JellyFin 3.6.5 remains the protected rollback baseline.

## Execution and trust boundaries

* All PS3 cross-compilation, PPU64 tests, static analysis and PKG verification
  run in GitHub Actions. Never download compilers, dependencies or artifacts to
  the user's Mac Mini.
* Windows OpenCode is a coding assistant, not the source of test truth. Use
  tools/opencode_harness.py to enforce independent validations, deadlines and
  immutable transcripts. The script refuses non-Windows execution. It never
  publishes a release, tags, pushes code or deploys a PKG.
* A write over FTP does not execute code. The native Jellyfin application must
  be running this new branch and must have an explicitly provisioned key.
* The only interface for console control is the JFR1 bounded command mailbox.
  No arbitrary scripts, shell commands, environment variables, paths, or
  remote code execution are accepted by the PS3 parser.
* A physical PS3 acceptance run is required before enabling new behaviors by
  default or describing the build as stable.

## Protocol version JFR1

Remote directory: /dev_hdd0/tmp/jf_remote

Provision \`auth.key\` with exactly 64 lowercase hexadecimal characters followed
by LF. Keep it private and never commit it. Do not rotate during playback.

Windows publishes a command to a unique temporary filename using FTP STOR,
then uses FTP RNFR/RNTO to publish \`command.ready\`. One command is allowed
at a time. The existing PS3 progress reporter polls only during Surf playback
at a 1-second period. The writer unlinks the command before dispatch to
prevent replay after reboot. The response is written to a temporary file,
flushed, closed and renamed to \`response.txt\`.

All wire messages are ASCII, LF-terminated, maximum 512 bytes, unique field
names, no control characters. Example command:

\`\`\`text
JFR1
id=0123456789abcdef
auth=<64 lowercase hex characters>
generation=123
action=get_status
\`\`\`

The response contains id, status (ok/error), detail, generation, mode,
free_kb and queue_depth. The Windows reader checks matching command ID.
Never silently retry a timed-out non-idempotent request. A command may have
been processed even if its response was not returned.

A non-query action must supply the exact active playback generation from a
recent \`get_status\` reply, never 0. The control code rechecks generation on the
display thread. Surf actions reuse the preexisting authenticated asynchronous
worker and its exact Previous selection token.

Supported commands:

| Command | Behavior |
|---|---|
| get_status | Return active generation, mode and free-memory snapshot |
| capture_metrics | Return status and force an additional binary sample |
| pause_surf / resume_surf | Control the current virtual channel |
| favorite_current | Favorite the current server-reported source |
| select_previous | Change the remembered Previous selection, not tune the channel |
| favorite_previous | Favorite the selected Previous using its server token |
| select_current | Clear Previous selection |
| set_audio_sync_beta | Set value=0/1 for the active session only |
| stop_playback | Request a clean exit from the active Surf playback |

The native implementation does not yet provide auto-launch from the PS3 XMB,
automatic initial selection of All/Sports/Movies & Series, or remote control
while the main menu is idle. Those require a separate UI lifecycle adapter
and must not be faked by uploading command files to the current 3.6.5 client.

## Compact local diagnostics

The worker samples a 32-byte big-endian \`JFM1\` binary record every five
seconds of active playback. The eight unsigned 32-bit fields, in order, are:

1. magic 0x4a464d31
2. playback generation
3. free system memory in KiB
4. jitter-buffer occupancy
5. audio clock in milliseconds
6. flags (bit zero = paused, bit one = forced capture_metrics sample)
7. monotonic millisecond timestamp (wraps modulo 2^32)
8. reserved zero

The files are \`/dev_hdd0/tmp/jf_metrics.bin\` and
\`/dev_hdd0/tmp/jf_metrics.prev.bin\`, bounded to approximately 1 MiB
each. These samples are independent of verbose player logging and do
not require remote HTTP requests. A five-second sample is intentionally
coarse: detailed source-epoch, flip and PCM synchronization diagnosis
remains future instrumentation.

Legacy crash logs and the optional verbose player log are preserved.
No new filesystem operations occur inside audio, decoder, upload, or
render hot paths. A command poll runs only on the progress reporter.
The remote control is fail-closed when the key is absent or malformed.

## Windows controller

Run from an existing Windows Python 3 working directory. The user must
install the native development build before any command can work.

\`\`\`powershell
python tools/ps3_unattended.py probe
python tools/ps3_unattended.py pair --secret-file C:\Jellyfin-Remote\ps3_mailbox.secret
python tools/ps3_unattended.py send get_status --secret-file C:\Jellyfin-Remote\ps3_mailbox.secret
python tools/ps3_unattended.py send pause_surf --generation 123 --secret-file C:\Jellyfin-Remote\ps3_mailbox.secret
python tools/ps3_unattended.py collect --output C:\Jellyfin-Remote\ps3_logs_next
python tools/ps3_unattended.py analyze C:\Jellyfin-Remote\ps3_logs_20261123\surf_hls.events.jsonl
\`\`\`

Use the actual generation returned by get_status instead of the example 123.
Do not run \`pair\` while the existing key is in use. The controller refuses to
replace an existing \`auth.key\`. Back up the key in a Windows-only secure
location, and never include it in diagnostic archives or Git commits.

## OpenCode engineering gate

Use \`tools/opencode_harness.py\` on Windows inside a clean Git checkout on a
feature/ps3-unattended-harness-* branch. The agent phases are audit,
implement, review and validate. An implementation is not accepted when
the OpenCode process times out, produces no output, changes files during a
read-only review, or independent Python/CLI checks fail.

\`\`\`powershell
python tools/opencode_harness.py --repo C:\path\to\JellyFin-PS3 --output C:\Jellyfin-Remote\opencode_evidence --phase all --timeout 300
\`\`\`

Use the independent GitHub CI gates after local OpenCode checks: full
host regression, AddressSanitizer/UBSan tests, PPC64 big-endian tests,
Cppcheck and the pinned PS3 GCC 7.2 cross-build. Do not publish or install
until those tests pass and the owner authorizes the installation.

## Physical acceptance checklist

On the next PS3 hardware session, first establish a baseline with the
existing 3.6.5 release and preserve its logs. For the new build, confirm:

1. No new boot or GPU flip stall beyond the measured baseline.
2. Remote status works with the exact ID and session generation.
3. Invalid key, malformed input, duplicate fields and stale generations
   are rejected without a crash.
4. Pause/resume and Favorite Current/Previous return actual server results.
5. A stopped or ended session cannot affect the next session.
6. The controller still works normally during command polling.
7. Audio and video are not regressed by the new logging.
8. Metrics logs rotate and retain correct record lengths and endian order.
9. Repeated playback open/close has no progressive memory decline.
10. Correlate PS3 diagnostics with server source changes and source quarantine.

Do not claim exact A/V sync is fixed from the mailbox alone. Precise
source-epoch attribution across the video decoder, RSX flips and audio
DMA ring remains an unresolved, separately gated task.
