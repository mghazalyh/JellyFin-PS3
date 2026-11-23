#!/usr/bin/env python3
"""Bounded FTP controller for JellyFin PS3's opt-in JFR1 mailbox.

No software is installed on the console by this program. A compatible native
client must already be running. The controller never executes arbitrary code.
"""
from __future__ import annotations

import argparse
import contextlib
import ftplib
import hashlib
import json
import os
import re
import secrets
import struct
import sys
import time
from collections import Counter, defaultdict
from datetime import datetime, timezone
from io import BytesIO
from pathlib import Path
from typing import Callable, Optional

MAGIC = "JFR1"
REMOTE_DIR = "/dev_hdd0/tmp/jf_remote"
KEY_PATH = REMOTE_DIR + "/auth.key"
COMMAND_PATH = REMOTE_DIR + "/command.ready"
RESPONSE_PATH = REMOTE_DIR + "/response.txt"
MAX_WIRE_BYTES = 512
MAX_ARCHIVE_BYTES = 8 * 1024 * 1024
ALLOWED_ACTIONS = frozenset({
    "get_status", "capture_metrics", "pause_surf", "resume_surf",
    "favorite_current", "select_previous", "favorite_previous",
    "select_current", "set_audio_sync_beta", "stop_playback",
})
HEX16 = re.compile(r"^[0-9a-f]{16}$")
HEX64 = re.compile(r"^[0-9a-f]{64}$")
KEY = re.compile(r"^[a-z][a-z0-9_]{0,31}$")
SAFE_VALUE = re.compile(r"^[\x20-\x7e]{0,160}$")


class MailboxError(RuntimeError):
    pass


def wire_encode(fields: dict[str, str]) -> bytes:
    if not fields or any(not KEY.fullmatch(k) or not SAFE_VALUE.fullmatch(v)
                         for k, v in fields.items()):
        raise MailboxError("invalid protocol field")
    result = (MAGIC + "\n" + "".join(f"{k}={v}\n" for k, v in fields.items())).encode("ascii")
    if len(result) > MAX_WIRE_BYTES:
        raise MailboxError("command exceeds 512-byte protocol limit")
    return result


def wire_decode(data: bytes) -> dict[str, str]:
    if not data or len(data) > MAX_WIRE_BYTES or b"\0" in data:
        raise MailboxError("invalid response length or binary content")
    try:
        raw = data.decode("ascii")
    except UnicodeError as exc:
        raise MailboxError("response is not ASCII") from exc
    if not raw.endswith("\n"):
        raise MailboxError("incomplete response")
    lines = raw.splitlines()
    if not lines or lines[0] != MAGIC:
        raise MailboxError("unexpected protocol version")
    fields: dict[str, str] = {}
    for line in lines[1:]:
        if "=" not in line:
            raise MailboxError("malformed response")
        name, value = line.split("=", 1)
        if not KEY.fullmatch(name) or not SAFE_VALUE.fullmatch(value) or name in fields:
            raise MailboxError("invalid or duplicate response field")
        fields[name] = value
    if not HEX16.fullmatch(fields.get("id", "")):
        raise MailboxError("response has invalid command ID")
    if fields.get("status") not in ("ok", "error"):
        raise MailboxError("response has invalid status")
    return fields


class PS3Mailbox:
    def __init__(self, host: str, port: int = 21, user: str = "anonymous",
                 password: str = "", timeout: float = 12.0,
                 ftp_factory: Callable[..., ftplib.FTP] = ftplib.FTP):
        if not host or timeout <= 0 or not 1 <= port <= 65535:
            raise ValueError("invalid FTP configuration")
        self.host, self.port, self.user, self.password = host, port, user, password
        self.timeout, self.ftp_factory = timeout, ftp_factory
        self.ftp: Optional[ftplib.FTP] = None

    def __enter__(self) -> "PS3Mailbox":
        ftp = self.ftp_factory()
        try:
            ftp.connect(self.host, self.port, timeout=self.timeout)
            ftp.login(self.user, self.password)
            self.ftp = ftp
            return self
        except Exception:
            with contextlib.suppress(Exception):
                ftp.close()
            raise

    def __exit__(self, *_: object) -> None:
        if self.ftp:
            with contextlib.suppress(Exception):
                self.ftp.quit()
            with contextlib.suppress(Exception):
                self.ftp.close()
            self.ftp = None

    def _ftp(self) -> ftplib.FTP:
        if self.ftp is None:
            raise MailboxError("not connected")
        return self.ftp

    def read(self, name: str, limit: int = MAX_WIRE_BYTES) -> bytes:
        if limit < 1:
            raise ValueError("invalid read limit")
        chunks: list[bytes] = []
        total = 0

        def collect(block: bytes) -> None:
            nonlocal total
            total += len(block)
            if total > limit:
                raise MailboxError(f"remote file exceeds {limit} bytes")
            chunks.append(block)

        self._ftp().retrbinary("RETR " + name, collect, blocksize=4096)
        return b"".join(chunks)

    def exists(self, name: str) -> bool:
        try:
            self._ftp().size(name)
            return True
        except ftplib.error_perm as exc:
            if str(exc).startswith(("550", "450")):
                return False
            raise

    def ensure_dir(self) -> None:
        try:
            self._ftp().mkd(REMOTE_DIR)
        except ftplib.error_perm:
            if not self.exists(REMOTE_DIR + "/.probe") and not self._can_cwd():
                raise MailboxError("cannot create or access remote mailbox directory")

    def _can_cwd(self) -> bool:
        ftp = self._ftp()
        current = ftp.pwd()
        try:
            ftp.cwd(REMOTE_DIR)
            return True
        except ftplib.error_perm:
            return False
        finally:
            with contextlib.suppress(Exception):
                ftp.cwd(current)

    def atomic_write(self, name: str, payload: bytes) -> None:
        if not payload:
            raise MailboxError("empty payload")
        temporary = name + "." + secrets.token_hex(6) + ".tmp"
        try:
            self._ftp().storbinary("STOR " + temporary, BytesIO(payload))
            self._ftp().rename(temporary, name)
        finally:
            with contextlib.suppress(ftplib.all_errors):
                self._ftp().delete(temporary)

    def pair(self, replace: bool = False) -> str:
        self.ensure_dir()
        if self.exists(KEY_PATH) and not replace:
            raise MailboxError("auth.key already exists; refusing to rotate it")
        secret = secrets.token_hex(32)
        self.atomic_write(KEY_PATH, (secret + "\n").encode("ascii"))
        print("PS3 mailbox paired. Store this secret securely; it will not be printed again.")
        return secret

    def send(self, action: str, secret: str, generation: int = 0,
             value: Optional[int] = None, response_timeout: float = 90,
             poll_interval: float = 0.5) -> dict[str, str]:
        if action not in ALLOWED_ACTIONS:
            raise MailboxError("command is not in the whitelist")
        if not HEX64.fullmatch(secret):
            raise MailboxError("secret must be 64 lowercase hex digits")
        if not isinstance(generation, int) or not 0 <= generation <= 0xffffffff:
            raise MailboxError("invalid generation")
        if action == "set_audio_sync_beta":
            if value not in (0, 1):
                raise MailboxError("set_audio_sync_beta requires value 0 or 1")
        elif value is not None:
            raise MailboxError("this action does not accept a value")
        if response_timeout <= 0 or poll_interval < 0.1:
            raise MailboxError("invalid timeout or polling interval")
        if self.exists(COMMAND_PATH):
            raise MailboxError("remote command slot occupied; refusing to overwrite")
        cmd_id = secrets.token_hex(8)
        fields = {
            "id": cmd_id, "auth": secret,
            "generation": str(generation), "action": action,
        }
        if value is not None:
            fields["value"] = str(value)
        self.atomic_write(COMMAND_PATH, wire_encode(fields))
        deadline = time.monotonic() + response_timeout
        errors = 0
        while time.monotonic() < deadline:
            time.sleep(min(poll_interval, max(0, deadline - time.monotonic())))
            try:
                response = wire_decode(self.read(RESPONSE_PATH))
                errors = 0
            except ftplib.error_perm as exc:
                if not str(exc).startswith(("550", "450")):
                    raise
                continue
            except MailboxError:
                errors += 1
                if errors >= 3:
                    raise
                continue
            if response.get("id") == cmd_id:
                if response["status"] != "ok":
                    raise MailboxError("PS3 rejected command: " + response.get("detail", "unspecified"))
                return response
        raise MailboxError("timed out; command outcome unknown, do not auto-retry")

    def collect(self, destination: Path) -> list[dict[str, object]]:
        destination.mkdir(parents=True, exist_ok=True)
        report: list[dict[str, object]] = []
        names = {
            "crash_log.txt": "/dev_hdd0/tmp/crash_log.txt",
            "update_detection.txt": "/dev_hdd0/tmp/update_detection.txt",
            "player_log.txt": "/dev_hdd0/tmp/player_log.txt",
            "player_log.prev.txt": "/dev_hdd0/tmp/player_log.prev.txt",
            "remote_response.txt": RESPONSE_PATH,
            "remote_metrics.bin": "/dev_hdd0/tmp/jf_metrics.bin",
            "remote_metrics.prev.bin": "/dev_hdd0/tmp/jf_metrics.prev.bin",
        }
        for local_name, remote_name in names.items():
            output = destination / local_name
            try:
                data = self.read(remote_name, MAX_ARCHIVE_BYTES)
                output.write_bytes(data)
                report.append({"file": local_name, "bytes": len(data),
                               "sha256": hashlib.sha256(data).hexdigest()})
            except ftplib.error_perm as exc:
                if str(exc).startswith(("550", "450")):
                    report.append({"file": local_name, "status": "missing"})
                else:
                    raise
            except MailboxError as exc:
                report.append({"file": local_name, "status": "skipped",
                               "reason": str(exc)})
        manifest = {"captured_utc": datetime.now(timezone.utc).isoformat(),
                    "console": self.host, "files": report}
        (destination / "manifest.json").write_text(
            json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        return report


def analyze_events(paths: list[Path]) -> dict[str, object]:
    counts: Counter[str] = Counter()
    failures: dict[str, Counter[str]] = defaultdict(Counter)
    malformed = total = 0
    for path in paths:
        with path.open("rt", encoding="utf-8", errors="replace") as stream:
            for line in stream:
                if len(line) > 100000:
                    malformed += 1
                    continue
                try:
                    event = json.loads(line)
                    if not isinstance(event, dict):
                        raise ValueError("non-object")
                except (ValueError, json.JSONDecodeError):
                    malformed += 1
                    continue
                name = str(event.get("event") or event.get("type") or "unknown")
                source = str(event.get("source") or event.get("channel") or "")
                counts[name] += 1
                total += 1
                if name in {"source_quarantined", "source_stalled", "ffmpeg_error",
                            "source_exited_without_media", "source_retired"} and source:
                    failures[source][name] += 1
    return {"events": total, "malformed": malformed, "by_event": dict(counts.most_common()),
            "by_failing_source": {k: dict(v) for k, v in
                                  sorted(failures.items(), key=lambda v: (-sum(v[1].values()), v[0]))}}



METRIC_WIRE = struct.Struct(">8I")
METRIC_MAGIC = 0x4A464D31


def analyze_metrics(paths: list[Path]) -> dict[str, object]:
    """Decode fixed 32-byte big-endian JFM1 records; never trust an input file."""
    sessions: dict[int, dict[str, object]] = {}
    malformed = truncated = samples = 0
    for path in paths:
        if path.stat().st_size > MAX_ARCHIVE_BYTES:
            raise MailboxError("metrics archive exceeds bounded maximum: " + str(path))
        with path.open("rb") as stream:
            while True:
                raw = stream.read(METRIC_WIRE.size)
                if not raw:
                    break
                if len(raw) != METRIC_WIRE.size:
                    truncated += len(raw)
                    break
                magic, generation, free_kb, queue, audio_ms, flags, clock_ms, reserved = (
                    METRIC_WIRE.unpack(raw)
                )
                if magic != METRIC_MAGIC or reserved != 0:
                    malformed += 1
                    continue
                entry = sessions.setdefault(generation, {
                    "samples": 0,
                    "first_free_kb": free_kb,
                    "last_free_kb": free_kb,
                    "min_free_kb": free_kb,
                    "max_free_kb": free_kb,
                    "min_queue": queue,
                    "max_queue": queue,
                    "first_audio_clock_ms": audio_ms,
                    "last_audio_clock_ms": audio_ms,
                    "paused_samples": 0,
                    "forced_samples": 0,
                    "elapsed_observed_ms": 0,
                    "clock_discontinuities": 0,
                    "_last_clock": None,
                })
                entry["samples"] += 1
                entry["last_free_kb"] = free_kb
                entry["min_free_kb"] = min(entry["min_free_kb"], free_kb)
                entry["max_free_kb"] = max(entry["max_free_kb"], free_kb)
                entry["min_queue"] = min(entry["min_queue"], queue)
                entry["max_queue"] = max(entry["max_queue"], queue)
                entry["last_audio_clock_ms"] = audio_ms
                entry["paused_samples"] += bool(flags & 1)
                entry["forced_samples"] += bool(flags & 2)
                last_clock = entry["_last_clock"]
                if last_clock is not None:
                    elapsed = (clock_ms - last_clock) & 0xffffffff
                    if elapsed <= 600000:
                        entry["elapsed_observed_ms"] += elapsed
                    else:
                        entry["clock_discontinuities"] += 1
                entry["_last_clock"] = clock_ms
                samples += 1
    for entry in sessions.values():
        entry.pop("_last_clock")
        entry["free_memory_delta_kb"] = entry["last_free_kb"] - entry["first_free_kb"]
    return {
        "total_samples": samples,
        "malformed_records": malformed,
        "truncated_trailing_bytes": truncated,
        "sessions": {str(k): v for k, v in sorted(sessions.items())},
    }



def run_soak(mailbox: PS3Mailbox, secret: str, destination: Path,
             duration_s: int, interval_s: int, exercise_controls: bool = False,
             clock: Callable[[], float] = time.monotonic,
             sleep: Callable[[float], None] = time.sleep) -> dict[str, object]:
    """Finite active-Surf test: no media selection or favorites modification."""
    if not 1 <= duration_s <= 3600 or not 15 <= interval_s <= 600:
        raise MailboxError("soak duration must be 1..3600s and interval 15..600s")
    destination.mkdir(parents=True, exist_ok=True)
    report: dict[str, object] = {
        "started_utc": datetime.now(timezone.utc).isoformat(),
        "max_duration_s": duration_s,
        "interval_s": interval_s,
        "exercise_controls": exercise_controls,
        "snapshots": [],
        "errors": [],
        "result": "running",
    }
    report_file = destination / "soak_report.json"
    deadline = clock() + duration_s
    controls_completed = False
    paused_by_us = False

    def save_report() -> None:
        report_file.write_text(json.dumps(report, indent=2) + "\n",
                               encoding="utf-8")

    try:
        # A status request also verifies that the PS3 is running this exact
        # native mailbox implementation and provides the session generation.
        first = mailbox.send("get_status", secret)
        generation = int(first.get("generation", "0"))
        mode = first.get("mode", "")
        if generation < 1 or mode not in ("all", "sports", "movies-series"):
            raise MailboxError("no eligible Surf playback session")
        report["generation"] = generation
        report["mode"] = mode
        save_report()
        while True:
            if clock() >= deadline and report["snapshots"]:
                break
            snapshot = mailbox.send("capture_metrics", secret)
            if (int(snapshot.get("generation", "0")) != generation or
                snapshot.get("mode") != mode):
                raise MailboxError("Surf session changed during unattended test")
            snapshot["elapsed_s"] = max(0, int(duration_s - max(0.0, deadline - clock())))
            report["snapshots"].append(snapshot)
            save_report()
            if exercise_controls and not controls_completed:
                mailbox.send("pause_surf", secret, generation=generation)
                paused_by_us = True
                try:
                    mailbox.send("resume_surf", secret, generation=generation)
                    paused_by_us = False
                finally:
                    # On failure, retry resume only as a best-effort state
                    # restoration; never retry a favorite or non-idempotent act.
                    if paused_by_us:
                        try:
                            mailbox.send("resume_surf", secret, generation=generation)
                            paused_by_us = False
                        except (MailboxError,) + ftplib.all_errors:
                            pass
                controls_completed = True
                report["controls_tested"] = ["pause_surf", "resume_surf"]
                save_report()
            remaining = deadline - clock()
            if remaining <= 0:
                break
            sleep(min(interval_s, remaining))
        report["result"] = "local_capture_complete"
    except (MailboxError,) + ftplib.all_errors as exc:
        report["result"] = "failed"
        report["errors"].append(f"{type(exc).__name__}: {exc}")
        raise
    finally:
        if paused_by_us:
            report["errors"].append("pause/resume restoration not confirmed")
        try:
            report["files"] = mailbox.collect(destination)
            present = [p for p in (
                destination / "remote_metrics.bin",
                destination / "remote_metrics.prev.bin"
            ) if p.exists()]
            if present:
                report["metrics"] = analyze_metrics(present)
        except (MailboxError,) + ftplib.all_errors as exc:
            report["errors"].append("diagnostic collection: " + str(exc))
        report["ended_utc"] = datetime.now(timezone.utc).isoformat()
        save_report()
    return report


def cli(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("PS3_HOST", "192.168.31.178"))
    parser.add_argument("--port", type=int, default=21)
    parser.add_argument("--timeout", type=float, default=12)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("probe", help="Check FTP login, without changing the console")
    pair = sub.add_parser("pair", help="Provision an opt-in mailbox key")
    pair.add_argument("--secret-file", type=Path, required=True,
                      help="Local 0600 file to store the generated key")
    send = sub.add_parser("send", help="Send one idempotent command")
    send.add_argument("action", choices=sorted(ALLOWED_ACTIONS))
    send.add_argument("--secret-file", type=Path, required=True)
    send.add_argument("--generation", type=int, default=0)
    send.add_argument("--value", type=int, choices=(0, 1))
    send.add_argument("--response-timeout", type=float, default=90)
    collect = sub.add_parser("collect", help="Save bounded diagnostic files")
    collect.add_argument("--output", type=Path, required=True)
    analyze = sub.add_parser("analyze", help="Aggregate existing server JSONL event logs offline")
    analyze.add_argument("paths", type=Path, nargs="+")
    metrics = sub.add_parser("metrics", help="Decode JFM1 PS3 binary metric archives offline")
    metrics.add_argument("paths", type=Path, nargs="+")
    soak = sub.add_parser("soak", help="Run a finite unattended Surf diagnostic test")
    soak.add_argument("--secret-file", type=Path, required=True)
    soak.add_argument("--output", type=Path, required=True)
    soak.add_argument("--duration", type=int, default=600)
    soak.add_argument("--interval", type=int, default=30)
    soak.add_argument("--exercise-controls", action="store_true")
    args = parser.parse_args(argv)
    try:
        if args.command == "analyze":
            print(json.dumps(analyze_events(args.paths), indent=2))
            return 0
        if args.command == "metrics":
            print(json.dumps(analyze_metrics(args.paths), indent=2))
            return 0
        with PS3Mailbox(args.host, port=args.port, timeout=args.timeout) as mailbox:
            if args.command == "probe":
                print(json.dumps({"ftp_connected": True, "host": args.host,
                                  "mailbox_present": mailbox._can_cwd()}))
            elif args.command == "pair":
                if args.secret_file.exists():
                    raise MailboxError("secret file already exists; refusing to overwrite")
                secret = mailbox.pair()
                fd = os.open(args.secret_file, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
                with os.fdopen(fd, "w", encoding="ascii") as output:
                    output.write(secret + "\n")
                print("Secret written to", args.secret_file)
            elif args.command == "send":
                secret = args.secret_file.read_text(encoding="ascii").strip()
                response = mailbox.send(args.action, secret, args.generation,
                                        args.value, args.response_timeout)
                print(json.dumps(response, indent=2))
            elif args.command == "collect":
                print(json.dumps(mailbox.collect(args.output), indent=2))
            elif args.command == "soak":
                secret = args.secret_file.read_text(encoding="ascii").strip()
                report = run_soak(mailbox, secret, args.output, args.duration,
                                  args.interval, args.exercise_controls)
                print(json.dumps({"result": report["result"],
                                  "snapshot_count": len(report["snapshots"]),
                                  "report": str(args.output / "soak_report.json")}, indent=2))
        return 0
    except (MailboxError,) + ftplib.all_errors as exc:
        print(f"ps3-harness: {type(exc).__name__}: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(cli())
