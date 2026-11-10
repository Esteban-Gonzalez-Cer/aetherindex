"""Run a checklist command without a shell and retain command, exit and both streams."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import shlex
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--id", required=True)
    parser.add_argument("--expected", type=int, default=0)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    assert re.fullmatch(r"[A-Za-z0-9_.-]+", args.id)
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    assert command
    folder = Path(__file__).resolve().parent
    root = folder.parents[3]
    started = datetime.now(timezone.utc).isoformat()
    begin = time.monotonic()
    try:
        result = subprocess.run(command, cwd=root, text=True, encoding="utf-8",
                                errors="replace", capture_output=True, timeout=300)
        status, stdout, stderr = result.returncode, result.stdout, result.stderr
    except subprocess.TimeoutExpired as error:
        status = -999
        stdout = error.stdout or b""
        stderr = error.stderr or b""
        if isinstance(stdout, bytes): stdout = stdout.decode("utf-8", errors="replace")
        if isinstance(stderr, bytes): stderr = stderr.decode("utf-8", errors="replace")
        stderr += "\nChecklist command exceeded 300 seconds.\n"
    duration = time.monotonic() - begin
    passed = status == args.expected
    (folder / f"{args.id}.stdout.txt").write_text(stdout)
    (folder / f"{args.id}.stderr.txt").write_text(stderr)
    entry = {"id": args.id, "working_directory": str(root), "command": command,
             "shell_command": shlex.join(command), "started_utc": started,
             "expected_returncode": args.expected, "actual_returncode": status,
             "passed": passed, "elapsed_seconds": duration,
             "stdout": f"{args.id}.stdout.txt", "stderr": f"{args.id}.stderr.txt",
             "transcript": f"{args.id}.log"}
    shown_stdout = stdout or "(empty)\n"
    shown_stderr = stderr or "(empty)\n"
    transcript = (f"Working directory: {root}\nCommand: {shlex.join(command)}\n"
                  f"Started UTC: {started}\nExpected exit: {args.expected}\nActual exit: {status}\n"
                  f"Test verdict: {'PASS' if passed else 'FAIL'}\n\n--- stdout ---\n"
                  f"{shown_stdout}\n--- stderr ---\n{shown_stderr}")
    (folder / f"{args.id}.log").write_text(transcript)
    manifest = folder / "commands.json"
    entries = json.loads(manifest.read_text()) if manifest.exists() else []
    entries = [row for row in entries if row["id"] != args.id]
    entries.append(entry)
    manifest.write_text(json.dumps(entries, indent=2) + "\n")
    print(f"{args.id}: {'PASS' if passed else 'FAIL'} (exit {status}, expected {args.expected})")
    if not passed: print(stdout[-3000:], stderr[-3000:])
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
