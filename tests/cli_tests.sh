#!/usr/bin/env bash
#
# End-to-end tests for the zhuzhbox CLI, driven against tests/mock_server.py so
# that nothing here needs the internet or uploads anything real.
#
# Usage: cli_tests.sh <path-to-zhuzhbox> [<path-to-mock_server.py>]
#
set -u

ZHUZHBOX=${1:?usage: cli_tests.sh <zhuzhbox binary> [mock_server.py]}
MOCK=${2:-$(dirname "$0")/mock_server.py}
PYTHON=${PYTHON:-python3}

# Everything below runs from a scratch directory, so hold absolute paths.
abspath() { "$PYTHON" -c 'import os,sys;print(os.path.abspath(sys.argv[1]))' "$1"; }
ZHUZHBOX=$(abspath "$ZHUZHBOX")
MOCK=$(abspath "$MOCK")

WORK=$(mktemp -d "${TMPDIR:-/tmp}/zb-tests.XXXXXX")
PASS=0
FAIL=0
SERVERS=()

cleanup() {
    for pid in "${SERVERS[@]:-}"; do
        [ -n "$pid" ] && kill "$pid" 2>/dev/null
    done
    rm -rf "$WORK"
}
trap cleanup EXIT

ok()   { PASS=$((PASS + 1)); printf '  ok   %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf '  FAIL %s\n' "$1"; [ $# -gt 1 ] && printf '       %s\n' "$2"; }
note() { printf '\n== %s\n' "$1"; }

check() { # check <description> <expected> <actual>
    if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "expected [$2], got [$3]"; fi
}

# start_mock <name> [extra args...]
#
# Sets MOCK_URL. Deliberately NOT called in a command substitution: that runs
# the function in a subshell, so the PID recorded in SERVERS would be lost and
# every run would orphan a server process holding its spooled uploads.
MOCK_URL=
start_mock() {
    local name=$1; shift
    local state="$WORK/$name.json"
    "$PYTHON" "$MOCK" --port 0 --state-file "$state" --spool-dir "$WORK/spool-$name" \
        "$@" >/dev/null 2>"$WORK/$name.log" &
    SERVERS+=("$!")
    for _ in $(seq 1 100); do
        [ -s "$state" ] && break
        sleep 0.1
    done
    if [ ! -s "$state" ]; then
        echo "could not start the mock server ($name)" >&2
        cat "$WORK/$name.log" >&2
        exit 1
    fi
    MOCK_URL="http://127.0.0.1:$("$PYTHON" -c "import json,sys;print(json.load(open(sys.argv[1]))['port'])" "$state")"
}

json_field() { "$PYTHON" -c 'import json,sys;d=json.load(open(sys.argv[1]))
for key in sys.argv[2:]:
    d = d[int(key)] if key.lstrip("-").isdigit() else d[key]
print(d)' "$@"; }

start_mock main
BASE=$MOCK_URL
export ZHUZHBOX_CONFIG_DIR="$WORK/config"
API=(--api "$BASE" --download-host "$BASE" --site "$BASE")
Z=("$ZHUZHBOX" "${API[@]}")

cd "$WORK" || exit 1

# ---------------------------------------------------------------- basics ----

note "basics"

"$ZHUZHBOX" --version >/dev/null 2>&1
check "--version exits 0" 0 $?

"$ZHUZHBOX" --help >/dev/null 2>&1
check "--help exits 0" 0 $?

"$ZHUZHBOX" nonsuchcommand >/dev/null 2>&1
check "unknown command exits 2" 2 $?

"$ZHUZHBOX" upload >/dev/null 2>&1
check "upload with no files exits 2" 2 $?

"${Z[@]}" health >/dev/null 2>&1
check "health exits 0 when the service is up" 0 $?

"${Z[@]}" health --json | "$PYTHON" -c 'import json,sys;assert json.load(sys.stdin)["status"]=="ok"'
check "health --json parses and says ok" 0 $?

"${Z[@]}" quota --json | "$PYTHON" -c 'import json,sys;d=json.load(sys.stdin);assert d["limitBytes"]>0'
check "quota --json parses" 0 $?

"${Z[@]}" stats --json | "$PYTHON" -c 'import json,sys;json.load(sys.stdin)["uploadsByDay"]'
check "stats --json parses" 0 $?

"$ZHUZHBOX" rules --json | "$PYTHON" -c 'import json,sys;d=json.load(sys.stdin);assert len(d["rules"])==5'
check "rules --json lists five rules" 0 $?

"$ZHUZHBOX" rules | grep -q "Short and non-negotiable"
check "rules prints the site's framing line" 0 $?

# --------------------------------------------------------------- uploads ----

note "uploads"

head -c 300000 /dev/urandom > sample.bin
head -c 40 /dev/urandom > tiny.bin

ZHUZHBOX_SINGLE_SHOT=off "${Z[@]}" upload sample.bin --no-resume -q --json > up1.json 2>up1.err
check "chunked upload exits 0" 0 $?
TOKEN=$(json_field up1.json uploads 0 token)
[ -n "$TOKEN" ] && ok "chunked upload returned a token" || bad "chunked upload returned a token"

SIZE=$(json_field up1.json uploads 0 size)
check "reported size matches the file" 300000 "$SIZE"

"${Z[@]}" upload tiny.bin -q --json > up2.json 2>/dev/null
check "single-shot upload exits 0" 0 $?
check "single-shot preserved the filename" "tiny.bin" "$(json_field up2.json uploads 0 filename)"

printf 'piped payload\n' > piped-expected.txt
"${Z[@]}" upload - --name piped.txt -q --json < piped-expected.txt > up3.json 2>/dev/null
check "upload from stdin exits 0" 0 $?
check "stdin upload used --name" "piped.txt" "$(json_field up3.json uploads 0 filename)"
check "stdin upload size matches the piped bytes" \
    "$(wc -c < piped-expected.txt | tr -d ' ')" "$(json_field up3.json uploads 0 size)"
"${Z[@]}" get "$(json_field up3.json uploads 0 token)" -o piped-back.txt -q -f
cmp -s piped-expected.txt piped-back.txt
check "the stdin upload round-trips byte-identically" 0 $?

ls "${TMPDIR:-/tmp}"/zhuzhbox-stdin-* >/dev/null 2>&1
check "the stdin temp file was cleaned up" 2 $?

"${Z[@]}" upload "$WORK" >/dev/null 2>&1
check "uploading a directory exits 2" 2 $?

"${Z[@]}" upload does-not-exist.bin >/dev/null 2>&1
check "uploading a missing file exits 1 or 2" 1 $(( $? == 2 ? 1 : $? ))

# ------------------------------------------------------------- downloads ----

note "downloads"

"${Z[@]}" get "$TOKEN" -o got.bin -q
check "get exits 0" 0 $?
cmp -s sample.bin got.bin
check "downloaded file is byte-identical" 0 $?

check "get -o - streams the right byte count" 300000 "$("${Z[@]}" get "$TOKEN" -o - -q | wc -c | tr -d ' ')"

"${Z[@]}" get "$TOKEN" -o got.bin -q >/dev/null 2>&1
check "get refuses to clobber without --force" 1 $?

"${Z[@]}" get "$TOKEN" -o got.bin -q --force
check "get --force overwrites" 0 $?

# Resume: seed a .part file with a correct prefix and let the CLI finish it.
rm -f resumed.bin resumed.bin.part
head -c 120000 sample.bin > resumed.bin.part
"${Z[@]}" get "$TOKEN" -o resumed.bin -q
check "ranged resume exits 0" 0 $?
cmp -s sample.bin resumed.bin
check "resumed download is byte-identical" 0 $?

"${Z[@]}" get zzzzzzzzzz -o missing.bin >/dev/null 2>&1
check "get on a missing token exits 1" 1 $?
[ ! -e missing.bin ] && ok "no file is left behind for a missing token" \
                     || bad "no file is left behind for a missing token"

"${Z[@]}" get "$TOKEN" 2>&1 | grep -q .
"${Z[@]}" get notatoken! >/dev/null 2>&1
check "get on a non-token exits 2" 2 $?

# --------------------------------------------------- hostile server names ----

note "untrusted filenames"

ESCAPE_TOKEN=$("$PYTHON" - "$BASE" <<'PY'
import json, sys, urllib.request
req = urllib.request.Request(sys.argv[1] + "/v1/sharex", data=b"x" * 32,
                             headers={"X-Filename": "../../../../tmp/zb-escaped.txt",
                                      "Content-Type": "text/plain"})
print(json.load(urllib.request.urlopen(req))["token"])
PY
)
rm -f /tmp/zb-escaped.txt
mkdir -p sandbox && (cd sandbox && "${Z[@]}" get "$ESCAPE_TOKEN" -q >/dev/null 2>&1)
[ -f sandbox/zb-escaped.txt ] && ok "traversal filename was reduced to a leaf name" \
                             || bad "traversal filename was reduced to a leaf name"
[ ! -f /tmp/zb-escaped.txt ] && ok "nothing was written outside the target directory" \
                             || bad "nothing was written outside the target directory"

WIN_TOKEN=$("$PYTHON" - "$BASE" <<'PY'
import json, sys, urllib.request
req = urllib.request.Request(sys.argv[1] + "/v1/sharex", data=b"y" * 16,
                             headers={"X-Filename": "C:\\Windows\\System32\\evil.dll",
                                      "Content-Type": "text/plain"})
print(json.load(urllib.request.urlopen(req))["token"])
PY
)
mkdir -p sandbox2 && (cd sandbox2 && "${Z[@]}" get "$WIN_TOKEN" -q >/dev/null 2>&1)
[ -f sandbox2/evil.dll ] && ok "windows path was reduced to a leaf name" \
                         || bad "windows path was reduced to a leaf name"

# ------------------------------------------------------------- shelf/ls ----

note "shelf"

"${Z[@]}" ls --json | "$PYTHON" -c 'import json,sys;d=json.load(sys.stdin);assert len(d)>=3'
check "ls lists the uploads from this run" 0 $?

"${Z[@]}" ls --json | grep -q deleteToken
check "ls --json hides delete tokens by default" 1 $?

"${Z[@]}" ls --json --reveal-tokens | grep -q deleteToken
check "ls --json --reveal-tokens includes them" 0 $?

"${Z[@]}" shelf show "$TOKEN" --json | "$PYTHON" -c 'import json,sys;assert json.load(sys.stdin)["deleteToken"]'
check "shelf show reveals the delete token" 0 $?

if [ "$(uname -s)" != "MINGW"* ]; then
    MODE=$(stat -c '%a' "$ZHUZHBOX_CONFIG_DIR/shelf.json" 2>/dev/null || stat -f '%Lp' "$ZHUZHBOX_CONFIG_DIR/shelf.json")
    check "shelf.json is mode 600" "600" "$MODE"
fi

for key in newest name size expires; do
    "${Z[@]}" ls --sort "$key" >/dev/null 2>&1 || bad "ls --sort $key"
done
ok "ls --sort accepts every documented key"
"${Z[@]}" ls --sort nonsense >/dev/null 2>&1
check "ls --sort rejects an unknown key with exit 2" 2 $?

"${Z[@]}" shelf export backup.json -q >/dev/null 2>&1
check "shelf export exits 0" 0 $?
BEFORE=$("${Z[@]}" ls --json --reveal-tokens | "$PYTHON" -c 'import json,sys;print(len(json.load(sys.stdin)))')

# Wipe the config directory, then restore from the backup and confirm the
# delete capability came back with it.
rm -rf "$ZHUZHBOX_CONFIG_DIR"
check "ls is empty after wiping the config dir" 0 "$("${Z[@]}" ls --json | "$PYTHON" -c 'import json,sys;print(len(json.load(sys.stdin)))')"
"${Z[@]}" shelf import backup.json >/dev/null 2>&1
check "shelf import exits 0" 0 $?
check "shelf import restored every entry" "$BEFORE" "$("${Z[@]}" ls --json | "$PYTHON" -c 'import json,sys;print(len(json.load(sys.stdin)))')"

# A hand-corrupted shelf must be reported and left alone.
cp "$ZHUZHBOX_CONFIG_DIR/shelf.json" good-shelf.json
printf '{"version": 1, "entries": [ this is not json' > "$ZHUZHBOX_CONFIG_DIR/shelf.json"
CORRUPT_BEFORE=$(cksum < "$ZHUZHBOX_CONFIG_DIR/shelf.json")
"${Z[@]}" ls >/dev/null 2>&1
check "a corrupt shelf makes ls exit 1" 1 $?
check "the corrupt shelf was left untouched" "$CORRUPT_BEFORE" "$(cksum < "$ZHUZHBOX_CONFIG_DIR/shelf.json")"
cp good-shelf.json "$ZHUZHBOX_CONFIG_DIR/shelf.json"

# A prune that cannot reach the server must change nothing.
SHELF_BEFORE=$(cksum < "$ZHUZHBOX_CONFIG_DIR/shelf.json")
"$ZHUZHBOX" --api http://127.0.0.1:1 ls >/dev/null 2>&1
check "prune failure still exits 0 (the list is just stale)" 0 $?
check "prune failure left the shelf byte-identical" "$SHELF_BEFORE" "$(cksum < "$ZHUZHBOX_CONFIG_DIR/shelf.json")"

# ----------------------------------------------------------- collections ----

note "collections"

printf 'one\n' > a.txt
printf 'two\n' > b.txt
printf 'три\n' > "юникод.txt"
"${Z[@]}" upload a.txt b.txt "юникод.txt" --bundle --title "Test bundle" \
    --description "three little files" -q --json > bundle.json 2>/dev/null
check "bundle upload exits 0" 0 $?
CTOKEN=$(json_field bundle.json collection token)
check "bundle contains three files" 3 "$(json_field bundle.json collection fileCount)"

"${Z[@]}" get "$CTOKEN" --json | "$PYTHON" -c 'import json,sys;d=json.load(sys.stdin);assert d["type"]=="collection";assert len(d["files"])==3'
check "get on a collection lists its members" 0 $?

[ ! -e a.txt.part ] && ok "listing a collection downloads nothing" \
                    || bad "listing a collection downloads nothing"

"${Z[@]}" get "$CTOKEN" --all -o bundle-out -q
check "get --all exits 0" 0 $?
cmp -s "юникод.txt" "bundle-out/юникод.txt"
check "a bundled UTF-8 filename round-trips" 0 $?

"${Z[@]}" upload a.txt --title "nope" >/dev/null 2>&1
check "--title without --bundle exits 2" 2 $?

"$PYTHON" -c 'print("x" * 201)' > long-title.txt
"${Z[@]}" upload a.txt --bundle --title "$(cat long-title.txt)" >/dev/null 2>&1
check "an over-long title is rejected locally with exit 2" 2 $?

# ---------------------------------------------------------------- delete ----

note "delete"

"${Z[@]}" rm "$CTOKEN" -y >/dev/null 2>&1
check "rm on a collection exits 0" 0 $?
"${Z[@]}" status "$CTOKEN" >/dev/null 2>&1
check "the deleted collection is now a 404" 1 $?
MEMBER=$(json_field bundle.json collection files 0 token)
"${Z[@]}" status "$MEMBER" >/dev/null 2>&1
check "deleting a collection cascaded to its members" 1 $?

"${Z[@]}" rm "$CTOKEN" --delete-token whatever -y >/dev/null 2>&1
check "rm on something already gone exits 0" 0 $?

"${Z[@]}" rm "$TOKEN" --delete-token definitelywrong -y >/dev/null 2>&1
check "rm with a wrong delete token exits 1" 1 $?
"${Z[@]}" ls --json | grep -q "$TOKEN"
check "a 403 leaves the shelf entry in place" 0 $?

"${Z[@]}" rm aaaaaaaaaa -y >/dev/null 2>&1
check "rm without a known delete token exits 2" 2 $?

"${Z[@]}" rm "$TOKEN" >/dev/null 2>&1 </dev/null
check "rm without --yes and without a TTY exits 2 rather than hanging" 2 $?

# ---------------------------------------------------------------- report ----

note "report"

REPORT_TOKEN=$(json_field up2.json uploads 0 token)
"${Z[@]}" report "$REPORT_TOKEN" --note "test report" --json > report1.json 2>/dev/null
check "report exits 0" 0 $?
"${Z[@]}" report "$BASE/d/$REPORT_TOKEN" --json > report2.json 2>/dev/null
check "report accepts a full link" 0 $?
check "a repeat report is flagged as a duplicate" "True" "$(json_field report2.json duplicate)"

"$PYTHON" -c 'print("x" * 2001)' > long-note.txt
"${Z[@]}" report "$REPORT_TOKEN" --note "$(cat long-note.txt)" >/dev/null 2>&1
check "an over-long note is rejected locally with exit 2" 2 $?

"${Z[@]}" report "not a link at all" >/dev/null 2>&1
check "report rejects a non-link with exit 2" 2 $?

# ---------------------------------------------------------------- config ----

note "config"

"${Z[@]}" config list --json | "$PYTHON" -c 'import json,sys;d=json.load(sys.stdin);assert d["apiHost"]["source"]'
check "config list --json reports a source per key" 0 $?

"$ZHUZHBOX" config set concurrency 5 >/dev/null 2>&1
check "config set exits 0" 0 $?
check "config get reads back what was set" "5" "$("$ZHUZHBOX" config get concurrency)"

"$ZHUZHBOX" config set concurrency 99 >/dev/null 2>&1
check "config set rejects an out-of-range value with exit 2" 2 $?
"$ZHUZHBOX" config get nosuchkey >/dev/null 2>&1
check "config get rejects an unknown key with exit 2" 2 $?

check "an env var overrides the config file" "environment" \
    "$(ZHUZHBOX_CONCURRENCY=7 "$ZHUZHBOX" config list --json | "$PYTHON" -c 'import json,sys;print(json.load(sys.stdin)["concurrency"]["source"])')"

# ------------------------------------------------------------- json/tty ----

note "output discipline"

for cmd in health quota stats rules "ls" "config list"; do
    # shellcheck disable=SC2086
    if "${Z[@]}" $cmd --json 2>/dev/null | "$PYTHON" -c 'import json,sys;json.load(sys.stdin)' 2>/dev/null; then
        :
    else
        bad "$cmd --json emits valid JSON on stdout"
    fi
done
ok "every read-only command emits valid JSON on stdout"

check "piped output carries no ANSI escapes" 0 \
    "$("${Z[@]}" ls | grep -c $'\033' || true)"

# ------------------------------------------------------------ resilience ----

note "resilience"

start_mock flaky --drop-rate 5
FLAKY=$MOCK_URL
head -c 62914560 /dev/urandom > big.bin   # 60 MiB, three chunks
ZHUZHBOX_SINGLE_SHOT=off "$ZHUZHBOX" --api "$FLAKY" --download-host "$FLAKY" \
    upload big.bin --no-resume -q --json > flaky.json 2>flaky.err
check "an upload survives one request in five being reset" 0 $?
if [ -s flaky.json ]; then
    FTOKEN=$(json_field flaky.json uploads 0 token)
    "$ZHUZHBOX" --api "$FLAKY" --download-host "$FLAKY" get "$FTOKEN" -o big-back.bin -q -f
    cmp -s big.bin big-back.bin
    check "the flaky upload is byte-identical on the way back" 0 $?
else
    bad "the flaky upload produced output" "$(head -3 flaky.err)"
fi

start_mock capacity --capacity-once
CAPACITY=$MOCK_URL
ZHUZHBOX_SINGLE_SHOT=off "$ZHUZHBOX" --api "$CAPACITY" --download-host "$CAPACITY" \
    upload tiny.bin --no-resume -q >/dev/null 2>&1
check "a 503 with Retry-After is waited out and retried" 0 $?

start_mock quota --quota-exhausted
QUOTA=$MOCK_URL
ZHUZHBOX_SINGLE_SHOT=off "$ZHUZHBOX" --api "$QUOTA" upload tiny.bin --no-resume \
    -q > /dev/null 2>quota.err
check "an exhausted quota exits 1" 1 $?
grep -qi "quota" quota.err
check "the server's own quota message is what gets printed" 0 $?

start_mock norange --ignore-range
NORANGE=$MOCK_URL
"$ZHUZHBOX" --api "$NORANGE" --download-host "$NORANGE" upload sample.bin -q --json \
    > norange.json 2>/dev/null
NRTOKEN=$(json_field norange.json uploads 0 token)
head -c 120000 sample.bin > nr.bin.part
"$ZHUZHBOX" --api "$NORANGE" --download-host "$NORANGE" get "$NRTOKEN" -o nr.bin -q
check "a server that ignores Range still yields a correct download" 0 $?
cmp -s sample.bin nr.bin
check "the stale partial prefix was discarded, not appended to" 0 $?

# ------------------------------------------------------------------ done ----

printf '\n%d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
