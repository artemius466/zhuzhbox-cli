#!/usr/bin/env bash
#
# Interrupt an upload with a real SIGINT, then finish it with --resume and
# check the bytes that came back. Split out from cli_tests.sh because it is
# the slowest test in the suite and the only one that depends on timing.
#
# Usage: resume_test.sh <path-to-zhuzhbox> [<path-to-mock_server.py>]
#
set -u

ZHUZHBOX=${1:?usage: resume_test.sh <zhuzhbox binary> [mock_server.py]}
MOCK=${2:-$(dirname "$0")/mock_server.py}
PYTHON=${PYTHON:-python3}

# See the note in cli_tests.sh: tolerate a stale interpreter path from CMake.
if ! "$PYTHON" -c '' >/dev/null 2>&1; then
    for candidate in python3 python; do
        if command -v "$candidate" >/dev/null 2>&1 &&
           "$candidate" -c '' >/dev/null 2>&1; then
            PYTHON=$candidate
            break
        fi
    done
fi

abspath() { "$PYTHON" -c 'import os,sys;print(os.path.abspath(sys.argv[1]))' "$1"; }
ZHUZHBOX=$(abspath "$ZHUZHBOX")
MOCK=$(abspath "$MOCK")

WORK=$(mktemp -d "${TMPDIR:-/tmp}/zb-resume.XXXXXX")
PASS=0
FAIL=0
SERVER_PID=

cleanup() {
    local i

    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null
    # See the matching comment in cli_tests.sh: give the killed process a
    # moment to release its open file handles (Windows) before removing the
    # directory, then tolerate a transient antivirus-scan lock with a few
    # retries rather than failing cleanup outright.
    if [ -n "$SERVER_PID" ]; then
        for i in 1 2 3 4 5; do
            kill -0 "$SERVER_PID" 2>/dev/null || break
            sleep 0.2
        done
    fi
    for i in 1 2 3 4 5; do
        rm -rf "$WORK" 2>/dev/null && break
        sleep 0.3
    done
    rm -rf "$WORK" 2>/dev/null || true
}
# INT/TERM/HUP as well as EXIT: a harness killed by a closed pipe or a
# ctest timeout must still take its mock servers down with it.
trap cleanup EXIT INT TERM HUP

ok()  { PASS=$((PASS + 1)); printf '  ok   %s\n' "$1"; }
bad() { FAIL=$((FAIL + 1)); printf '  FAIL %s\n' "$1"; [ $# -gt 1 ] && printf '       %s\n' "$2"; }
check() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "expected [$2], got [$3]"; fi; }

# Half a second per chunk gives a comfortable window to interrupt in.
"$PYTHON" "$MOCK" --port 0 --state-file "$WORK/mock.json" --chunk-delay 0.5 \
    --spool-dir "$WORK/spool" >/dev/null 2>"$WORK/mock.log" &
SERVER_PID=$!
for _ in $(seq 1 150); do
    [ -s "$WORK/mock.json" ] && break
    kill -0 "$SERVER_PID" 2>/dev/null || break
    sleep 0.1
done
if [ ! -s "$WORK/mock.json" ]; then
    {
        echo "could not start the mock server"
        echo "  python:  $PYTHON"
        "$PYTHON" --version 2>&1 | sed 's/^/  version: /'
        echo "  script:  $MOCK"
        echo "  --- server log ---"
        cat "$WORK/mock.log" 2>/dev/null || echo "  (no log)"
    } >&2
    exit 1
fi
BASE="http://127.0.0.1:$("$PYTHON" -c 'import json,sys;print(json.load(open(sys.argv[1]))["port"])' "$WORK/mock.json")"

export ZHUZHBOX_CONFIG_DIR="$WORK/config"
cd "$WORK" || exit 1

# Six chunks at 20 MiB, so an interrupt lands somewhere in the middle.
head -c 115343360 /dev/urandom > big.bin
EXPECTED=$("$PYTHON" -c '
import hashlib, sys
h = hashlib.sha256()
with open("big.bin", "rb") as f:
    for block in iter(lambda: f.read(1 << 20), b""):
        h.update(block)
print(h.hexdigest())')

echo "== interrupt"

ZHUZHBOX_SINGLE_SHOT=off "$ZHUZHBOX" --api "$BASE" --download-host "$BASE" \
    upload big.bin --no-resume -q > up.json 2>up.err &
UPLOAD_PID=$!

# Wait until at least one chunk has actually landed, then interrupt.
#
# This used to ask Python how many chunks the session records, spawning a
# fresh interpreter every 0.1s. That is cheap on Linux/macOS but Windows'
# process creation is dramatically slower (CI runners commonly see 100ms+ per
# python.exe start, versus single-digit ms for a POSIX fork+exec) — spent
# across up to 300 iterations, that overhead alone could burn the entire
# budget before a single chunk ever got the chance to land, which looks
# identical to a genuine stall from the outside. Watching the session file's
# size grow is just as decisive (an empty sentChunks array serializes far
# smaller than one holding an index) and costs nothing but a stat() call.
sessions_path="$ZHUZHBOX_CONFIG_DIR/sessions.json"
baseline_size=0
[ -s "$sessions_path" ] && baseline_size=$(wc -c < "$sessions_path" | tr -d ' ')
for _ in $(seq 1 300); do
    if [ -s "$sessions_path" ]; then
        current_size=$(wc -c < "$sessions_path" | tr -d ' ')
        # +5 is comfortably past the byte or two a lone digit costs, so this
        # doesn't fire on formatting noise alone.
        [ "$current_size" -gt $((baseline_size + 5)) ] 2>/dev/null && break
    fi
    # Give up early if the upload died rather than waiting out the whole loop.
    kill -0 "$UPLOAD_PID" 2>/dev/null || break
    sleep 0.1
done
sleep 0.5
kill -INT "$UPLOAD_PID" 2>/dev/null
wait "$UPLOAD_PID"
RC=$?

check "SIGINT during an upload exits 130" 130 "$RC"

if [ -s "$ZHUZHBOX_CONFIG_DIR/sessions.json" ]; then
    ok "a session file was left behind"
else
    bad "a session file was left behind"
fi

SENT=$("$PYTHON" -c '
import json, sys
data = json.load(open(sys.argv[1]))
sessions = data["sessions"]
print(len(sessions[0]["sentChunks"]) if sessions else 0)' "$ZHUZHBOX_CONFIG_DIR/sessions.json" 2>/dev/null || echo 0)

if [ "$SENT" -gt 0 ]; then
    ok "the session records $SENT chunk(s) already sent"
else
    bad "the session records progress" "sentChunks was $SENT"
fi

grep -qi "resume" up.err
check "the interrupt message tells you how to continue" 0 $?

echo "== resume"

ZHUZHBOX_SINGLE_SHOT=off "$ZHUZHBOX" --api "$BASE" --download-host "$BASE" \
    upload big.bin --resume -q --json > up2.json 2>up2.err
check "the resumed upload exits 0" 0 $?

TOKEN=$("$PYTHON" -c 'import json,sys;print(json.load(open("up2.json"))["uploads"][0]["token"])' 2>/dev/null)
if [ -n "$TOKEN" ]; then
    ok "the resumed upload produced a token"
else
    bad "the resumed upload produced a token" "$(head -3 up2.err)"
fi

REMAINING=$("$PYTHON" -c '
import json, sys
try:
    print(len(json.load(open(sys.argv[1]))["sessions"]))
except Exception:
    print(0)' "$ZHUZHBOX_CONFIG_DIR/sessions.json")
check "the finished session was dropped" 0 "$REMAINING"

"$ZHUZHBOX" --api "$BASE" --download-host "$BASE" get "$TOKEN" -o back.bin -q -f
check "downloading the resumed upload exits 0" 0 $?

ACTUAL=$("$PYTHON" -c '
import hashlib
h = hashlib.sha256()
with open("back.bin", "rb") as f:
    for block in iter(lambda: f.read(1 << 20), b""):
        h.update(block)
print(h.hexdigest())')
check "the resumed upload hashes identically to the source" "$EXPECTED" "$ACTUAL"

printf '\n%d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
