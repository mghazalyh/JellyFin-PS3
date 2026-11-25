import importlib.util
import json
import pathlib
import tempfile
import unittest
from unittest.mock import patch

MODULE = pathlib.Path(__file__).resolve().parents[1] / "tools" / "ps3_unattended.py"
spec = importlib.util.spec_from_file_location("ps3_unattended", MODULE)
h = importlib.util.module_from_spec(spec)
spec.loader.exec_module(h)


class FakeFTP:
    def __init__(self):
        self.files = {}
        self.directories = {h.REMOTE_DIR}
        self.connected = False
        self.answer = "ok"

    def connect(self, host, port, timeout):
        assert host and port == 21 and timeout > 0
        self.connected = True

    def login(self, user, password):
        assert self.connected

    def quit(self):
        self.connected = False

    def close(self):
        self.connected = False

    def pwd(self):
        return "/"

    def cwd(self, path):
        if path not in self.directories and path != "/":
            raise h.ftplib.error_perm("550 missing")

    def mkd(self, path):
        if path in self.directories:
            raise h.ftplib.error_perm("550 already exists")
        self.directories.add(path)

    def size(self, name):
        if name not in self.files:
            raise h.ftplib.error_perm("550 missing")
        return len(self.files[name])

    def retrbinary(self, command, callback, blocksize=4096):
        name = command.removeprefix("RETR ")
        if name not in self.files:
            raise h.ftplib.error_perm("550 missing")
        for start in range(0, len(self.files[name]), blocksize):
            callback(self.files[name][start:start + blocksize])

    def storbinary(self, command, stream):
        self.files[command.removeprefix("STOR ")] = stream.read()

    def rename(self, a, b):
        self.files[b] = self.files.pop(a)
        if b == h.COMMAND_PATH:
            command = self.files.pop(b).decode("ascii")
            command_id = next(line[3:] for line in command.splitlines()
                              if line.startswith("id="))
            self.files[h.RESPONSE_PATH] = h.wire_encode({
                "id": command_id, "status": self.answer,
                "detail": "verified action", "generation": "123", "mode": "all",
            })

    def delete(self, name):
        if name not in self.files:
            raise h.ftplib.error_perm("550 missing")
        del self.files[name]


class HarnessTests(unittest.TestCase):
    def test_valid_wire_and_duplicate(self):
        packet = h.wire_encode({"id": "0123456789abcdef", "status": "ok"})
        self.assertEqual(h.wire_decode(packet)["status"], "ok")
        with self.assertRaises(h.MailboxError):
            h.wire_decode(packet + b"status=error\n")
        with self.assertRaises(h.MailboxError):
            h.wire_decode(packet[:-1])
        with self.assertRaises(h.MailboxError):
            h.wire_decode(b"JFR0\nid=0123456789abcdef\nstatus=ok\n")
        with self.assertRaises(h.MailboxError):
            h.wire_encode({"bad-key": "value"})
        with self.assertRaises(h.MailboxError):
            h.wire_encode({"id": "\nmalicious"})

    def test_bounded_fully_verified_command(self):
        fake = FakeFTP()
        with h.PS3Mailbox("127.0.0.1", ftp_factory=lambda: fake) as controller:
            with patch.object(h.secrets, "token_hex", side_effect=[
                "0123456789abcdef", "abcdef123456"
            ]):
                result = controller.send("get_status", "a" * 64,
                                         response_timeout=1, poll_interval=0.1)
            self.assertEqual(result["id"], "0123456789abcdef")
            self.assertEqual(result["status"], "ok")
            self.assertNotIn(h.COMMAND_PATH, fake.files)
            fake.answer = "error"
            with self.assertRaises(h.MailboxError):
                controller.send("pause_surf", "a" * 64, generation=123,
                                response_timeout=1, poll_interval=0.1)

    def test_reject_unknown_unsafe_and_stale_queue(self):
        fake = FakeFTP()
        with h.PS3Mailbox("127.0.0.1", ftp_factory=lambda: fake) as controller:
            with self.assertRaises(h.MailboxError):
                controller.send("rm -rf", "a" * 64)
            with self.assertRaises(h.MailboxError):
                controller.send("get_status", "bad")
            with self.assertRaises(h.MailboxError):
                controller.send("set_audio_sync_beta", "a" * 64, value=2)
            with self.assertRaises(h.MailboxError):
                controller.send("pause_surf", "a" * 64, generation=-1)
            fake.files[h.COMMAND_PATH] = b"unfinished"
            with self.assertRaises(h.MailboxError):
                controller.send("get_status", "a" * 64)

    def test_pair_refuses_replacement(self):
        fake = FakeFTP()
        with h.PS3Mailbox("127.0.0.1", ftp_factory=lambda: fake) as controller:
            secret = controller.pair()
            self.assertEqual(fake.files[h.KEY_PATH], (secret + "\n").encode())
            with self.assertRaises(h.MailboxError):
                controller.pair()

    def test_archive_size_bound(self):
        fake = FakeFTP()
        fake.files["/dev_hdd0/tmp/crash_log.txt"] = b"crash debug"
        fake.files["/dev_hdd0/tmp/player_log.txt"] = b"A" * (h.MAX_ARCHIVE_BYTES + 1)
        with tempfile.TemporaryDirectory() as tmp:
            with h.PS3Mailbox("127.0.0.1", ftp_factory=lambda: fake) as controller:
                report = controller.collect(pathlib.Path(tmp))
            self.assertTrue((pathlib.Path(tmp) / "crash_log.txt").exists())
            self.assertFalse((pathlib.Path(tmp) / "player_log.txt").exists())
            self.assertTrue((pathlib.Path(tmp) / "manifest.json").exists())
            self.assertEqual(next(e for e in report if e["file"] == "player_log.txt")["status"],
                             "skipped")

    def test_binary_metrics_big_endian_and_wrap(self):
        with tempfile.TemporaryDirectory() as folder:
            log = pathlib.Path(folder) / "jf_metrics.bin"
            wire = h.METRIC_WIRE
            rows = [
                (h.METRIC_MAGIC, 7, 15000, 4, 250, 0, 0xfffffffe, 0),
                (h.METRIC_MAGIC, 7, 14800, 2, 550, 1, 5, 0),
                (h.METRIC_MAGIC, 7, 14900, 1, 850, 2, 100, 0),
                (h.METRIC_MAGIC, 8, 20000, 0, 5, 0, 200, 0),
            ]
            log.write_bytes(b"".join(wire.pack(*row) for row in rows))
            report = h.analyze_metrics([log])
            self.assertEqual(report["total_samples"], 4)
            self.assertEqual(report["malformed_records"], 0)
            session = report["sessions"]["7"]
            self.assertEqual(session["samples"], 3)
            self.assertEqual(session["min_free_kb"], 14800)
            self.assertEqual(session["free_memory_delta_kb"], -100)
            self.assertEqual(session["elapsed_observed_ms"], 102)
            self.assertEqual(session["paused_samples"], 1)
            self.assertEqual(session["forced_samples"], 1)
            self.assertEqual(report["sessions"]["8"]["samples"], 1)
            log.write_bytes(log.read_bytes() + b"BAD")
            report = h.analyze_metrics([log])
            self.assertEqual(report["truncated_trailing_bytes"], 3)
            with self.assertRaises(h.MailboxError):
                log.write_bytes(b"x" * (h.MAX_ARCHIVE_BYTES + 1))
                h.analyze_metrics([log])

    def test_jfm2_clock_epochs_and_invalid_records(self):
        with tempfile.TemporaryDirectory() as folder:
            log = pathlib.Path(folder) / "jf_metrics2.bin"
            def row(mono, audio, epoch=1, connection=0, flags=161, starves=0):
                def split(v):
                    return v >> 32, v & 0xffffffff
                return h.METRIC2_WIRE.pack(h.METRIC2_MAGIC, 7, flags,
                    connection, epoch, 10, 10440, 8, 100+starves, starves,
                    *split(mono), *split(audio), *split(500), *split(mono-100),
                    0, 0)
            base = 0x100000000
            log.write_bytes(row(base, 10_000_000) +
                row(base+10_000_000, 15_000_000, starves=2) +
                row(base+20_000_000, 0, starves=3) +
                row(base+30_000_000, 2_000_000, epoch=2, starves=4) +
                row(base+40_000_000, 100, epoch=2, connection=1) +
                b"x"*80+b"bad")
            report = h.analyze_metrics2([log])
            self.assertEqual(report["total_samples"], 5)
            self.assertEqual(report["malformed_records"], 1)
            self.assertEqual(report["truncated_trailing_bytes"], 3)
            first = report["sessions"][f"{log}:7:0"]
            self.assertEqual(first["backward_audio_steps"], 1)
            self.assertEqual(first["abnormal_clock_intervals"], 1)
            self.assertEqual(first["observed_epoch_changes"], 1)
            self.assertEqual(first["starvation_blocks_delta"], 4)
            self.assertEqual(first["max_picture_age_us"], 100)

    def test_unattended_soak_is_bounded_and_archived(self):
        fake = FakeFTP()
        with tempfile.TemporaryDirectory() as folder:
            output = pathlib.Path(folder) / "evidence"
            clock_value = [0.0]

            def clock():
                return clock_value[0]

            def sleep(seconds):
                clock_value[0] += seconds

            with h.PS3Mailbox("127.0.0.1", ftp_factory=lambda: fake) as controller:
                result = h.run_soak(controller, "a" * 64, output,
                                    duration_s=16, interval_s=15,
                                    exercise_controls=True,
                                    clock=clock, sleep=sleep)
            self.assertEqual(result["result"], "local_capture_complete")
            self.assertEqual(result["generation"], 123)
            self.assertEqual(result["mode"], "all")
            self.assertEqual(len(result["snapshots"]), 2)
            self.assertEqual(result["controls_tested"], ["pause_surf", "resume_surf"])
            self.assertTrue((output / "soak_report.json").exists())
            self.assertTrue((output / "manifest.json").exists())

    def test_soak_input_limits(self):
        fake = FakeFTP()
        with tempfile.TemporaryDirectory() as folder:
            with h.PS3Mailbox("127.0.0.1", ftp_factory=lambda: fake) as controller:
                with self.assertRaises(h.MailboxError):
                    h.run_soak(controller, "a" * 64, pathlib.Path(folder), 3601, 30)
                with self.assertRaises(h.MailboxError):
                    h.run_soak(controller, "a" * 64, pathlib.Path(folder), 60, 1)

    def test_streaming_offline_analysis(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "surf.events.jsonl"
            path.write_text(
                '{"event":"source_quarantined","source":"broken"}\n'
                '{"event":"source_stalled","source":"broken"}\n'
                '{"event":"source_ready","source":"good"}\n'
                'not json\n', encoding="utf-8")
            result = h.analyze_events([path])
            self.assertEqual(result["events"], 3)
            self.assertEqual(result["malformed"], 1)
            self.assertEqual(result["by_failing_source"]["broken"]["source_quarantined"], 1)


if __name__ == "__main__":
    unittest.main()
