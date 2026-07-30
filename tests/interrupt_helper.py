#!/usr/bin/env python3
"""Run a command, wait until it records real progress, then interrupt it the
way the host platform actually delivers Ctrl+C. Reports what happened as JSON.

Why this exists instead of `kill -INT` in the test script:

POSIX signals and Windows console control events are different mechanisms,
and zhuzhbox handles them separately -- sigaction(SIGINT) on POSIX,
SetConsoleCtrlHandler on Windows. A POSIX-style `kill -INT` from an MSYS or
Git-bash shell does NOT generate a console control event for a *native* Win32
child: MSYS emulates signals among its own processes, but for a native child
it falls back to TerminateProcess. That kills the process outright with no
handler, no flush, and no graceful shutdown -- which looks exactly like
"stderr was empty and the session recorded nothing", while the shell still
reports 130 by its own 128+signal convention, so even the exit-code check
passes misleadingly. The same unreliability applies to `kill -0` for liveness
polling, which makes wait-loops built on it break immediately.

So the interrupt is delivered here instead:

  POSIX    proc.send_signal(SIGINT) -- an ordinary SIGINT.
  Windows  the child is spawned in its own process group and sent
           CTRL_BREAK_EVENT. CTRL_BREAK is the only console control event
           that can be aimed at a single process group rather than every
           process sharing the console, and zhuzhbox's handler treats it the
           same as CTRL_C.

Either way the program takes its normal graceful path and exits 130 as an
ordinary exit code, so the caller compares against 130 on both platforms.
"""

import argparse
import json
import os
import signal
import subprocess
import sys
import time


def count_sent_chunks(path):
    """Chunks the first session records, or 0 if that cannot be read yet.

    The file is rewritten via write-temp-then-rename after every chunk, so a
    read can legitimately land on a missing or partial file; that is not an
    error, just "no answer yet".
    """
    try:
        with open(path, "r", encoding="utf-8") as handle:
            sessions = json.load(handle)["sessions"]
    except (OSError, ValueError, KeyError):
        return 0
    if not sessions:
        return 0
    return len(sessions[0].get("sentChunks", []))


def deliver_interrupt(proc):
    """Interrupt `proc`. Returns (mechanism, error) -- error is None on success."""
    if os.name == "nt":
        try:
            # Reaches the child's SetConsoleCtrlHandler; TerminateProcess and
            # POSIX-emulated kill do not.
            os.kill(proc.pid, signal.CTRL_BREAK_EVENT)
            return "ctrl_break", None
        except (OSError, AttributeError, ValueError) as exc:
            # Generating a console control event needs an attached console.
            # Some CI environments run without one, in which case the
            # graceful path genuinely cannot be exercised here -- report that
            # rather than silently mis-testing something else.
            return None, "GenerateConsoleCtrlEvent failed: %s" % exc
    try:
        proc.send_signal(signal.SIGINT)
        return "sigint", None
    except OSError as exc:
        return None, "send_signal(SIGINT) failed: %s" % exc


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--sessions", required=True,
                        help="path to sessions.json to watch")
    parser.add_argument("--stdout", required=True)
    parser.add_argument("--stderr", required=True)
    parser.add_argument("--min-chunks", type=int, default=1,
                        help="wait for at least this many chunks to land")
    parser.add_argument("--timeout", type=float, default=90.0,
                        help="give up waiting for progress after this long")
    parser.add_argument("--settle", type=float, default=0.3,
                        help="pause after progress appears, so the interrupt "
                             "lands mid-transfer rather than exactly on a "
                             "chunk boundary")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()

    command = args.command
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        sys.stderr.write("interrupt_helper: no command given\n")
        return 2

    creationflags = 0
    if os.name == "nt":
        # Required for CTRL_BREAK_EVENT to be targetable at this child alone
        # instead of everything attached to the console (this script
        # included).
        creationflags = subprocess.CREATE_NEW_PROCESS_GROUP

    result = {
        "delivered": None,
        "delivery_error": None,
        "chunks_at_interrupt": 0,
        "exit_code": None,
        "waited_seconds": 0.0,
        "progress_seen": False,
    }

    with open(args.stdout, "wb") as out, open(args.stderr, "wb") as errf:
        proc = subprocess.Popen(command, stdout=out, stderr=errf,
                                creationflags=creationflags)

        started = time.time()
        while time.time() - started < args.timeout:
            if proc.poll() is not None:
                break  # exited on its own; nothing left to interrupt
            if count_sent_chunks(args.sessions) >= args.min_chunks:
                result["progress_seen"] = True
                break
            time.sleep(0.05)
        result["waited_seconds"] = round(time.time() - started, 2)

        if proc.poll() is None:
            if result["progress_seen"] and args.settle > 0:
                time.sleep(args.settle)
            result["chunks_at_interrupt"] = count_sent_chunks(args.sessions)
            mechanism, error = deliver_interrupt(proc)
            result["delivered"] = mechanism
            result["delivery_error"] = error
            if mechanism is None:
                proc.kill()

        try:
            result["exit_code"] = proc.wait(timeout=60)
        except subprocess.TimeoutExpired:
            proc.kill()
            result["exit_code"] = proc.wait()
            result["delivery_error"] = (result["delivery_error"]
                                        or "process ignored the interrupt")

    # Re-read afterwards: the graceful path flushes the session on its way
    # out, so the post-exit count is the one that matters for resumability.
    result["chunks_after_exit"] = count_sent_chunks(args.sessions)

    json.dump(result, sys.stdout)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
