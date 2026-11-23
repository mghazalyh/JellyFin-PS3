#!/usr/bin/env python3
"""Fail-closed OpenCode job supervisor for the JellyFin PS3 Windows workspace.

Run on Windows next to a Git checkout, never on the user's Mac Mini.
OpenCode makes proposals and local changes; independent checks decide whether
they are acceptable. This supervisor does not push, tag, deploy, or publish.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

ALLOWED_CHANGE_PATHS = (
    "source/player/", "tests/", "tools/",
    "docs/ps3-unattended-harness.md",
    ".github/workflows/ps3-harness.yml",
)

MAX_OUTPUT = 8 * 1024 * 1024
ALLOWED_BRANCH_PREFIX = "feature/ps3-unattended-harness-"
PHASES = ("audit", "implement", "review", "validate")
INSTRUCTIONS = (
    "You are working on JellyFin PS3, not an ordinary desktop application. "
    "Read AGENTS.md, .clinerules, docs/DEVELOPMENT.md, and "
    "docs/ps3-unattended-harness.md before touching code. "
    "Protect 3.6.5 as rollback. No pushes, releases, deployment, PS3 FTP "
    "writes, Windows service restarts, or development files on Mac Mini. "
    "Do not invent PSL1GHT APIs. Keep the implementation bounded, use the "
    "existing progress worker and Surf worker, never block CellAudio or RSX. "
    "Do not claim hardware validation. Return exact files, diagnostics, tests "
    "and unresolved issues. If a test cannot be run, say so."
)
PROMPTS = {
    "audit": (
        "READ ONLY. Trace remote command ownership across "
        "source/player/remote_protocol.h, remote_mailbox.cpp, player.cpp, "
        "player_menu.cpp and player_threads.cpp. Inspect telemetry and "
        "source generation races. Report reproducible defects in priority "
        "order with file/line references. Do not edit."
    ),
    "implement": (
        "Implement fixes for confirmed correctness, memory, concurrency, "
        "input-validation or lifecycle defects in the unattended harness. "
        "Do not modify RSX or low-level audio paths speculatively. Add "
        "host-testable regressions for every fixed bug. Keep changes minimal "
        "and record all relevant verification outputs. No pushing or release."
    ),
    "review": (
        "READ ONLY adversarial review. Look for use-after-free, data races, "
        "memory fragmentation, unchecked filesystem results, mailbox replay, "
        "forged replies, PS3 build incompatibility, excess polling, memory "
        "drift, mismatched responses, source-token mixups and no-fail cleanup. "
        "Cross-check claimed passes against actual test results. Do not edit."
    ),
}


class HarnessError(RuntimeError):
    pass


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def run(argv: list[str], cwd: Path, timeout: int, log: Path,
        max_output: int = MAX_OUTPUT) -> dict[str, object]:
    if timeout < 1 or max_output < 1:
        raise ValueError("invalid execution limit")
    log.parent.mkdir(parents=True, exist_ok=True)
    stderr = log.with_suffix(".stderr.log")
    start = time.monotonic()
    proc = None
    reason = ""
    try:
        with log.open("wb") as out, stderr.open("wb") as err:
            # No shell: arguments are never interpolated as commands.
            proc = subprocess.Popen(argv, cwd=str(cwd), stdin=subprocess.DEVNULL,
                                    stdout=out, stderr=err, shell=False)
            deadline = start + timeout
            while proc.poll() is None:
                if (log.stat().st_size + stderr.stat().st_size) > max_output:
                    reason = "output_limit"
                    proc.kill()
                    break
                if time.monotonic() >= deadline:
                    reason = "timeout"
                    proc.kill()
                    break
                time.sleep(0.2)
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
    except (OSError, subprocess.SubprocessError) as exc:
        reason = "process_error:" + type(exc).__name__
    output = log.read_bytes() if log.exists() else b""
    error = stderr.read_bytes() if stderr.exists() else b""
    return {
        "argv0": argv[0], "args": argv[1:3],
        "ok": bool(proc and proc.returncode == 0 and not reason),
        "returncode": proc.returncode if proc else None,
        "reason": reason,
        "elapsed_seconds": round(time.monotonic() - start, 2),
        "stdout_bytes": len(output), "stderr_bytes": len(error),
        "stdout_sha256": hashlib.sha256(output).hexdigest(),
        "stderr_sha256": hashlib.sha256(error).hexdigest(),
        "stdout_tail": output[-2000:].decode("utf-8", errors="replace"),
        "stderr_tail": error[-2000:].decode("utf-8", errors="replace"),
    }


def read_opencode_result(log: Path) -> dict[str, object]:
    """Require parseable OpenCode event JSONL and at least one actual answer."""
    text_parts = 0
    errors = 0
    events = 0
    with log.open("rt", encoding="utf-8", errors="replace") as stream:
        for line in stream:
            if not line.strip():
                continue
            try:
                event = json.loads(line)
            except (json.JSONDecodeError, ValueError) as exc:
                raise HarnessError("OpenCode output is not valid JSONL") from exc
            if not isinstance(event, dict):
                raise HarnessError("OpenCode event is not a JSON object")
            events += 1
            kind = event.get("type")
            if kind == "error" or event.get("error"):
                errors += 1
            part = event.get("part")
            if isinstance(part, dict):
                if part.get("type") == "text" and isinstance(part.get("text"), str):
                    if part["text"].strip():
                        text_parts += 1
                state = part.get("state")
                if isinstance(state, dict) and state.get("status") == "error":
                    errors += 1
    if errors or not text_parts:
        raise HarnessError("OpenCode returned errors or no substantive answer")
    return {"events": events, "answer_parts": text_parts, "error_events": errors}


def changed_paths(status: str) -> list[str]:
    paths = []
    for raw in status.splitlines():
        if len(raw) < 4:
            raise HarnessError("invalid git status output")
        path = raw[3:]
        if (" -> " in path or '"' in path or "\\" in path or
            path.startswith("/") or path.startswith("../") or
            ".." in path.split("/")):
            raise HarnessError("unsafe or ambiguous changed path")
        if not any(path.startswith(prefix) for prefix in ALLOWED_CHANGE_PATHS):
            raise HarnessError("OpenCode changed a file outside allowlisted locations: " + path)
        paths.append(path)
    return paths


def git_check(repo: Path, logdir: Path, timeout: int = 15) -> tuple[str, str]:
    def git(*cmd: str) -> str:
        r = run(["git", *cmd], repo, timeout, logdir / ("git_" + cmd[0] + ".log"))
        if not r["ok"]:
            raise HarnessError("git " + cmd[0] + " failed: " + str(r["stderr_tail"]))
        return (logdir / ("git_" + cmd[0] + ".log")).read_text(encoding="utf-8").strip()
    root = Path(git("rev-parse", "--show-toplevel")).resolve()
    if root != repo.resolve():
        raise HarnessError("working tree root differs from requested repository")
    branch = git("branch", "--show-current")
    if not branch.startswith(ALLOWED_BRANCH_PREFIX):
        raise HarnessError("refusing unsafe branch: " + branch)
    status = git("status", "--porcelain")
    return branch, status


def independent_validation(repo: Path, out: Path, timeout: int) -> dict[str, object]:
    checks = {
        "python_compile": [sys.executable, "-m", "py_compile",
                           "tools/ps3_unattended.py", "tools/opencode_harness.py",
                           "tests/test_ps3_unattended.py",
                           "tests/test_opencode_harness.py"],
        "python_unittest": [sys.executable, "-m", "unittest", "discover",
                            "-s", "tests", "-p", "test_*harness.py", "-v"],
        "ftp_unittest": [sys.executable, "-m", "unittest", "discover",
                         "-s", "tests", "-p", "test_ps3_unattended.py", "-v"],
        "git_diff_check": ["git", "diff", "--check"],
    }
    results = {}
    for name, command in checks.items():
        results[name] = run(command, repo, timeout, out / (name + ".log"))
    return results


def execute(repo: Path, out: Path, phases: list[str],
            opencode: str, timeout: int, check_timeout: int) -> dict[str, object]:
    out.mkdir(parents=True, exist_ok=True)
    branch, initial_status = git_check(repo, out)
    initial_rev = run(["git", "rev-parse", "HEAD"], repo, 15, out / "initial_head.log")
    if not initial_rev["ok"]:
        raise HarnessError("cannot establish immutable initial Git HEAD")
    base_head = (out / "initial_head.log").read_text(encoding="ascii").strip()
    if initial_status and "implement" in phases:
        raise HarnessError("implementation stage requires a clean working tree")
    manifest: dict[str, object] = {
        "started_utc": utc_now(), "branch": branch,
        "initial_status": initial_status, "initial_head": base_head,
        "phases": {},
        "push_performed": False, "deployment_performed": False,
    }
    try:
        for phase in phases:
            if phase not in PHASES:
                raise HarnessError("invalid phase")
            if phase == "validate":
                result = independent_validation(repo, out, check_timeout)
                manifest["phases"][phase] = result
                if not all(r["ok"] for r in result.values()):
                    raise HarnessError("independent validation failed")
            else:
                before = git_check(repo, out)[1]
                prompt = INSTRUCTIONS + "\n\n" + PROMPTS[phase]
                command = [opencode, "run", "--agent",
                           "build" if phase == "implement" else "plan",
                           "--format", "json", prompt]
                result = run(command, repo, timeout, out / (phase + ".jsonl"))
                manifest["phases"][phase] = result
                if not result["ok"] or result["stdout_bytes"] == 0:
                    raise HarnessError("OpenCode " + phase + " did not complete successfully")
                evidence = read_opencode_result(out / (phase + ".jsonl"))
                manifest["phases"][phase + "_events"] = evidence
                after = git_check(repo, out)[1]
                current_rev = run(["git", "rev-parse", "HEAD"], repo, 15,
                                  out / (phase + "_head.log"))
                if not current_rev["ok"] or (out / (phase + "_head.log")).read_text().strip() != base_head:
                    raise HarnessError("OpenCode created or changed Git HEAD unexpectedly")
                if phase != "implement" and after != before:
                    raise HarnessError("read-only agent modified the checkout")
                if phase == "implement":
                    changed = changed_paths(after)
                    if not changed:
                        raise HarnessError("OpenCode implement returned without any source changes")
                    manifest["phases"]["modified_files"] = changed
                    validation = independent_validation(repo, out, check_timeout)
                    manifest["phases"]["post_implement_validation"] = validation
                    if not all(r["ok"] for r in validation.values()):
                        raise HarnessError("implementation failed independent checks")
        manifest["result"] = "local_checks_passed"
        manifest["note"] = "Physical PS3 stability and GitHub pinned cross-build remain unverified"
    except HarnessError as exc:
        manifest["result"] = "failed"
        manifest["error"] = str(exc)
    manifest["ended_utc"] = utc_now()
    (out / "report.json").write_text(json.dumps(manifest, indent=2) + "\n",
                                     encoding="utf-8")
    return manifest


def main(argv: Optional[list[str]] = None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--repo", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--phase", choices=(*PHASES, "all"), default="audit")
    p.add_argument("--opencode", default="opencode")
    p.add_argument("--timeout", type=int, default=300)
    p.add_argument("--check-timeout", type=int, default=60)
    args = p.parse_args(argv)
    if os.name != "nt":
        print("This harness is reserved for the Windows development machine; no Mac execution.",
              file=sys.stderr)
        return 2
    phases = list(PHASES) if args.phase == "all" else [args.phase]
    try:
        report = execute(args.repo.resolve(), args.output.resolve(),
                         phases, args.opencode, args.timeout, args.check_timeout)
        print(json.dumps({"result": report["result"], "report": str(args.output / "report.json")}))
        return 0 if report["result"] == "local_checks_passed" else 1
    except (HarnessError, OSError, ValueError) as exc:
        print("opencode-harness: " + str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
