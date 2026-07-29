# zhuzhbox CLI

A heavily vibecoded command-line client for [zhuzhbox](https://zhuzhbox.fun), the anonymous
no-signup file host. Written in C11, depends on libcurl and nothing else.

```bash
zhuzhbox upload holiday.mp4
zhuzhbox upload *.png --bundle --title "Screenshots"
zhuzhbox ls
zhuzhbox get https://zhuzhbox.fun/d/abc123XYZ
zhuzhbox rm abc123XYZ
```

## There is no account, so the shelf matters

zhuzhbox has no sign-in, and uploads are encrypted at rest with keys derived
from the token in the share link. The server genuinely cannot tell you which
uploads are yours, and cannot reissue a delete token.

That makes `shelf.json`, in your config directory, the **only** record of what
you have shared and the only place your delete tokens live. Back it up:

```bash
zhuzhbox shelf export ~/zhuzhbox-backup.json
```

The export is created readable only by you, but it is not encrypted on any
platform. Treat it like a password file.

## Install

### From source

Requires a C11 compiler, CMake ≥ 3.16, and libcurl development headers.

| Platform | Dependencies |
|---|---|
| Debian / Ubuntu | `sudo apt install build-essential cmake libcurl4-openssl-dev` |
| Fedora / RHEL | `sudo dnf install gcc cmake libcurl-devel` |
| Arch | `sudo pacman -S base-devel cmake curl` |
| Alpine | `sudo apk add build-base cmake curl-dev` |
| macOS | `brew install cmake curl` |
| Windows | `vcpkg install curl:x64-windows` (or use the prebuilt binary) |

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build
```

Or, if you prefer typing less, `make && sudo make install` — the `Makefile` is
a wrapper around those commands

### Prebuilt binaries

Each release carries `linux-x86_64`, `linux-aarch64`, `macos-universal` and
`windows-x86_64` builds with SHA-256 sums

## Commands

| Command | What it does |
|---|---|
| `upload <file...>` | Upload each file separately, or as one bundle with `--bundle` |
| `get <link\|token>` | Download a file; list a collection, or fetch it all with `--all` |
| `ls` | List what this machine has uploaded, pruning anything the server no longer has |
| `rm <link\|token>` | Delete an upload (and, for a bundle, everything in it) |
| `status <link\|token>` | Show what a link points at, without downloading it |
| `quota` | Your rolling weekly upload allowance |
| `stats` | Service-wide totals |
| `health` | Whether the service is up; exits 0 only if it is |
| `report <link>` | Report an upload for breaking the rules |
| `rules` | The content rules |
| `config <get\|set\|list>` | Persisted settings |
| `shelf <export\|import\|show>` | Back up and restore your uploads list |

`zhuzhbox <command> --help` has the details for any of them.

## Global options

| Flag | Effect |
|---|---|
| `--json` | Emit JSON on stdout instead of formatted text. Every command supports it. |
| `-q`, `--quiet` | Suppress progress bars and status lines. Errors still go to stderr. |
| `--no-color` | Disable ANSI color. `NO_COLOR` and a non-TTY stdout do this automatically. |
| `--api URL` | Override the API base URL for this invocation only |
| `--download-host URL` | Override the download host |
| `--site URL` | Override the site host used when printing links |
| `--config DIR` | Use a different config/shelf directory |
| `--debug` | Verbose diagnostics on stderr. Delete tokens are redacted. |

Exit codes: `0` success, `1` a handled error, `2` a usage error, `130`
interrupted.

## Limits

| Rule | Value |
|---|---|
| Max file size | 25 GiB (checked locally, before any network call) |
| Files per collection | 200 |
| Collection title / description | 200 / 4000 characters |
| Report note | 2000 characters |
| Weekly quota | 30 GiB, rolling 7 days, counting only files still live |
| Upload rate limit | 20 upload or collection inits per 15 minutes |
| Idle upload session | discarded after 3 hours |

Retention depends on size: under 5 GiB keeps for 30 days, 5 GiB and up for 15
days, 15 GiB and up for 7 days, 20 GiB and up for 3 days. Files in a bundle
inherit the bundle's expiry, so a collection never loses one member early.

## Scripting notes

`--json` puts a single JSON value on stdout and nothing else; progress and
diagnostics always go to stderr.

```bash
zhuzhbox health --json | jq -r .status
zhuzhbox upload build.tar.gz --json | jq -r '.uploads[0].url'
zhuzhbox ls --json | jq -r '.[] | select(.kind == "collection") | .token'
```

Piping works in both directions:

```bash
tar cz dist | zhuzhbox upload - --name dist.tar.gz
zhuzhbox get abc123XYZ -o - | tar xz
```

`zhuzhbox upload -` buffers stdin to a private temporary file first, because
the API needs an exact size before the upload starts and a pipe cannot be
measured or rewound. The temp file is created `0600` and removed on the way
out, including on Ctrl+C.

Anything that would prompt (`rm` without `--yes`) fails with an actionable
message instead of hanging when there is no terminal to ask on.

## Interrupted uploads

Progress is written to `sessions.json` after every chunk, so Ctrl+C during a
20 GB upload costs you the current chunk and nothing more:

```bash
zhuzhbox upload big.iso        # ^C somewhere in the middle
zhuzhbox upload big.iso --resume
```

Resuming asks the server which chunks it actually holds and sends only the
gaps; the server is treated as authoritative when it disagrees with the local
record. `--no-resume` always starts over. Sessions the server has already
discarded (after 3 hours idle) are pruned automatically.

Interrupted downloads work the same way: bytes land in `<target>.part` and are
renamed into place only when the download is complete, so an interrupted run
never leaves something that looks finished. Re-running resumes with a Range
request; if the server answers `200` instead of `206` the partial file is
discarded rather than spliced onto a different response.

## Configuration

Settings resolve most-specific-first: command-line flags, then `ZHUZHBOX_*`
environment variables, then `config.json`, then the built-in defaults.
`zhuzhbox config list` shows each value and where it came from.

| Key | Environment variable | Default |
|---|---|---|
| `apiHost` | `ZHUZHBOX_API_HOST` | `https://api.zhuzhbox.fun` |
| `downloadHost` | `ZHUZHBOX_DOWNLOAD_HOST` | `https://dl.zhuzhbox.fun` |
| `siteHost` | `ZHUZHBOX_SITE_HOST` | `https://zhuzhbox.fun` |
| `concurrency` | `ZHUZHBOX_CONCURRENCY` | `3` |
| `bundle` | `ZHUZHBOX_BUNDLE` | `off` |
| `color` | `ZHUZHBOX_COLOR` | `auto` |
| `progress` | `ZHUZHBOX_PROGRESS` | `auto` |
| `singleShot` | `ZHUZHBOX_SINGLE_SHOT` | `on` |
| `singleShotMax` | `ZHUZHBOX_SINGLE_SHOT_MAX` | `20 MiB` |
| `resume` | `ZHUZHBOX_RESUME` | `off` |

Files live in `$XDG_CONFIG_HOME/zhuzhbox` (Linux), `~/Library/Application
Support/zhuzhbox` (macOS) or `%APPDATA%\zhuzhbox` (Windows). Override the whole
directory with `--config` or `ZHUZHBOX_CONFIG_DIR`.

## Building and testing

```bash
make test            # build and run the whole suite under CTest
make test-sanitize   # the same suite under ASan + UBSan
```

Nothing in the suite touches the network or uploads anything real: everything
runs against `tests/mock_server.py`, a small fixture that speaks the same API.
It can also simulate the failures worth testing — dropped connections, a `503`
with `Retry-After`, an exhausted quota, a server that ignores `Range`.

CMake options: `ZB_SANITIZE`, `ZB_STATIC_CURL`, `ZB_WERROR`, `ZB_BUILD_TESTS`.

### Content rules

`zhuzhbox rules` prints text baked into the binary at build time, because no
API endpoint serves it. `src/rules_generated.h` is committed so building never
needs the website checkout, and `tools/gen_rules --check` fails CI if the two
drift apart.

```bash
make rules   # regenerate from a website checkout
```

## Not included

No TUI, no daemon or watch-folder mode, no accounts, no telemetry, no
auto-updater, no ShareX config generator (this *is* the scriptable client), and
no `--zip` flag, which would mean taking on a compression dependency.

## License

MIT. Vendored cJSON is MIT too — see `third_party/cJSON/LICENSE`.
