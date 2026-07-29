#!/usr/bin/env python3
"""A small stand-in for the zhuzhbox API, speaking the contract in section 3
of the build spec so that `ctest` needs no internet and no real uploads.

It is deliberately dumb: everything lives in memory, there is no crypto, and
the only cleverness is the failure injection the tests need.

Payloads are spooled to temporary files rather than held in memory, and
--max-upload caps how large an upload it will accept at all. Both exist because
a test that points a multi-gigabyte upload at an in-memory mock will exhaust
the machine's RAM long before it exercises anything interesting.

  --port N          listen here (0 picks a free port, printed on stdout)
  --drop-rate N     fail one request in N with a connection reset
  --capacity-once   answer the first /upload/init with 503 + Retry-After
  --quota-exhausted answer /upload/init with the 429 quota payload
  --ignore-range    answer ranged downloads with 200 instead of 206
  --chunk-delay S   stall each chunk PUT, so a test can interrupt mid-upload
  --max-upload N    refuse an upload larger than this many bytes (default 512M)
  --state-file P    write {"port": N} once listening, for test harnesses
"""

import argparse
import atexit
import json
import os
import re
import signal
import secrets
import shutil
import socket
import struct
import sys
import tempfile
import threading
import time
from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

CHUNK_SIZE = 20 * 1024 * 1024
MAX_UPLOAD_BYTES = 25 * 1024 * 1024 * 1024
MAX_FILES_PER_COLLECTION = 200
QUOTA_BYTES = 30 * 1024 * 1024 * 1024
WINDOW_DAYS = 7

STATE = {
    "sessions": {},     # token -> dict
    "files": {},        # token -> dict
    "collections": {},  # token -> dict
    "reports": set(),
    "request_count": 0,
}
LOCK = threading.Lock()
OPTS = None


def now_iso(offset_days=0):
    stamp = datetime.now(timezone.utc) + timedelta(days=offset_days)
    return stamp.strftime("%Y-%m-%dT%H:%M:%S.") + f"{stamp.microsecond // 1000:03d}Z"


def retention_days(size):
    for threshold, days in ((20, 3), (15, 7), (5, 15)):
        if size >= threshold * 1024 * 1024 * 1024:
            return days
    return 30


def new_token(n=10):
    alphabet = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
    return "".join(secrets.choice(alphabet) for _ in range(n))


SPOOL_DIR = None


class Blob:
    """Upload payload backed by a temp file, so the server's memory use stays
    flat no matter how big the upload is."""

    def __init__(self, size=0):
        self.path = tempfile.mkstemp(prefix="zb-mock-", dir=SPOOL_DIR)[1]
        self.size = size
        if size:
            with open(self.path, "wb") as handle:
                handle.truncate(size)

    def write_at(self, offset, data):
        with open(self.path, "r+b") as handle:
            handle.seek(offset)
            handle.write(data)

    def append(self, data):
        with open(self.path, "ab") as handle:
            handle.write(data)
        self.size += len(data)

    def read(self, offset=0, length=None):
        with open(self.path, "rb") as handle:
            handle.seek(offset)
            return handle.read() if length is None else handle.read(length)

    def discard(self):
        try:
            os.unlink(self.path)
        except OSError:
            pass


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "zhuzhbox-mock/1"

    def log_message(self, fmt, *args):
        if os.environ.get("ZB_MOCK_VERBOSE"):
            sys.stderr.write("mock: " + (fmt % args) + "\n")

    # ---- helpers -----------------------------------------------------

    def send_json(self, status, payload, extra_headers=None):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        for key, value in (extra_headers or {}).items():
            self.send_header(key, value)
        self.end_headers()
        self.wfile.write(body)

    def send_error_json(self, status, message, extra=None, headers=None):
        payload = {"error": message}
        payload.update(extra or {})
        self.send_json(status, payload, headers)

    def read_body(self):
        length = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(length) if length else b""

    def read_json(self):
        raw = self.read_body()
        if not raw:
            return {}
        try:
            return json.loads(raw.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            return None

    def should_drop(self):
        """Simulate a flaky network: kill the connection mid-request."""
        if not OPTS.drop_rate:
            return False
        with LOCK:
            STATE["request_count"] += 1
            count = STATE["request_count"]
        return count % OPTS.drop_rate == 0

    def drop(self):
        # Send a real RST rather than a graceful close, so the client sees the
        # abrupt failure a dropped connection actually looks like instead of
        # sitting on a half-open socket until its stall timer fires.
        try:
            self.close_connection = True
            self.connection.setsockopt(
                socket.SOL_SOCKET, socket.SO_LINGER,
                struct.pack("ii", 1, 0))
            self.connection.close()
        except OSError:
            pass

    # ---- routing -----------------------------------------------------

    def do_GET(self):
        if self.should_drop():
            return self.drop()
        path = self.path.split("?", 1)[0]

        if path == "/v1/health":
            return self.send_json(200, {"status": "ok", "uptimeSeconds": 4242})

        if path == "/v1/quota":
            used = sum(f["size"] for f in STATE["files"].values())
            return self.send_json(200, {
                "usedBytes": used,
                "limitBytes": QUOTA_BYTES,
                "remainingBytes": max(0, QUOTA_BYTES - used),
                "windowDays": WINDOW_DAYS,
            })

        if path == "/v1/stats":
            total = sum(f["size"] for f in STATE["files"].values())
            return self.send_json(200, {
                "totalBytes": total,
                "totalFiles": len(STATE["files"]),
                "totalCollections": len(STATE["collections"]),
                "disk": {"totalBytes": 1 << 40, "freeBytes": (1 << 40) - total},
                "uploadsByDay": [
                    {"date": "2026-07-28", "files": 1, "bytes": 1024},
                    {"date": "2026-07-29", "files": len(STATE["files"]),
                     "bytes": total},
                ],
                "generatedAt": now_iso(),
            })

        match = re.fullmatch(r"/v1/upload/([A-Za-z0-9]+)/status", path)
        if match:
            session = STATE["sessions"].get(match.group(1))
            if session is None:
                return self.send_error_json(404, "Not found or expired.")
            return self.send_json(200, {
                "totalChunks": session["totalChunks"],
                "chunkSize": session["chunkSize"],
                "size": session["size"],
                "receivedChunks": sorted(session["received"]),
            })

        match = re.fullmatch(r"/v1/upload/([A-Za-z0-9]+)", path)
        if match:
            return self.serve_metadata(match.group(1))

        match = re.fullmatch(r"/d/([A-Za-z0-9]+)", path)
        if match:
            return self.serve_download(match.group(1))

        return self.send_error_json(404, "Not found or expired.")

    def serve_metadata(self, token):
        record = STATE["collections"].get(token)
        if record is not None:
            files = [STATE["files"][t] for t in record["files"]
                     if t in STATE["files"]]
            return self.send_json(200, {
                "token": token,
                "type": "collection",
                "title": record["title"],
                "description": record["description"],
                "files": [{"token": f["token"], "filename": f["filename"],
                           "mimeType": f["mimeType"], "size": f["size"]}
                          for f in files],
                "fileCount": len(files),
                "totalSize": sum(f["size"] for f in files),
                "uploadedAt": record["uploadedAt"],
                "expiresAt": record["expiresAt"],
            })

        record = STATE["files"].get(token)
        if record is None:
            return self.send_error_json(404, "Not found or expired.")
        return self.send_json(200, {
            "token": token,
            "type": "file",
            "filename": record["filename"],
            "size": record["size"],
            "mimeType": record["mimeType"],
            "uploadedAt": record["uploadedAt"],
            "expiresAt": record["expiresAt"],
        })

    def serve_download(self, token):
        record = STATE["files"].get(token)
        if record is None:
            return self.send_error_json(404, "Not found or expired.")
        blob = record["blob"]
        total = blob.size
        start = 0
        ranged = False

        header = self.headers.get("Range")
        if header and not OPTS.ignore_range:
            match = re.fullmatch(r"bytes=(\d+)-(\d*)", header.strip())
            if match:
                start = int(match.group(1))
                if start < total:
                    ranged = True
                else:
                    start = 0

        payload = blob.read(start if ranged else 0)
        self.send_response(206 if ranged else 200)
        self.send_header("Content-Type", record["mimeType"])
        self.send_header("Content-Length", str(len(payload)))
        # HTTP header values are latin-1; anything else goes in the RFC 5987
        # form, which is what a real server would send too.
        ascii_name = record["filename"].encode("ascii", "replace").decode()
        self.send_header(
            "Content-Disposition",
            'attachment; filename="%s"' % ascii_name.replace('"', ""))
        if ranged:
            self.send_header("Content-Range", "bytes %d-%d/%d"
                             % (start, total - 1, total))
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()
        self.wfile.write(payload)

    def do_POST(self):
        if self.should_drop():
            return self.drop()
        path = self.path.split("?", 1)[0]

        if path == "/v1/upload/init":
            return self.upload_init()
        if path == "/v1/sharex":
            return self.sharex()
        if path == "/v1/upload/collection/init":
            return self.collection_init()
        if path == "/v1/uploads/exists":
            return self.uploads_exists()
        if path == "/v1/reports":
            return self.reports()

        match = re.fullmatch(r"/v1/upload/([A-Za-z0-9]+)/complete", path)
        if match:
            return self.upload_complete(match.group(1))

        match = re.fullmatch(r"/v1/upload/collection/([A-Za-z0-9]+)/complete",
                             path)
        if match:
            return self.collection_complete(match.group(1))

        return self.send_error_json(404, "Not found or expired.")

    def upload_init(self):
        body = self.read_json()
        if body is None:
            return self.send_error_json(400, "Invalid JSON body.")

        if OPTS.quota_exhausted:
            used = QUOTA_BYTES - 1024
            return self.send_error_json(
                429, "Weekly upload quota exhausted.",
                {"usedBytes": used, "limitBytes": QUOTA_BYTES,
                 "remainingBytes": 1024, "windowDays": WINDOW_DAYS})

        if OPTS.capacity_once:
            with LOCK:
                if not STATE.get("capacity_used"):
                    STATE["capacity_used"] = True
                    return self.send_error_json(
                        503, "Server is at upload capacity, try again shortly.",
                        headers={"Retry-After": "1"})

        filename = body.get("filename")
        size = body.get("size")
        if not filename or not isinstance(filename, str):
            return self.send_error_json(400, "A filename is required.")
        if not isinstance(size, (int, float)) or size < 0:
            return self.send_error_json(400, "A valid size is required.")
        size = int(size)
        if size > MAX_UPLOAD_BYTES:
            return self.send_error_json(413, "File exceeds the 25 GB limit.")
        # A guard rail for the test suite, not part of the real API: this
        # process would otherwise happily spool tens of gigabytes.
        if size > OPTS.max_upload:
            return self.send_error_json(
                413, "This mock server refuses uploads over %d bytes; raise "
                     "--max-upload if a test really needs that."
                     % OPTS.max_upload)

        collection_token = body.get("collectionToken")
        if collection_token is not None:
            collection = STATE["collections"].get(collection_token)
            if collection is None:
                return self.send_error_json(404, "Not found or expired.")
            if collection["sealed"]:
                return self.send_error_json(409, "This collection is sealed.")
            if len(collection["files"]) >= MAX_FILES_PER_COLLECTION:
                return self.send_error_json(
                    400, "This collection already holds the maximum of "
                         "%d files." % MAX_FILES_PER_COLLECTION)

        token = new_token()
        total_chunks = max(1, (size + CHUNK_SIZE - 1) // CHUNK_SIZE)
        with LOCK:
            STATE["sessions"][token] = {
                "token": token,
                "deleteToken": new_token(24),
                "filename": filename,
                "mimeType": body.get("mimeType") or "application/octet-stream",
                "size": size,
                "chunkSize": CHUNK_SIZE,
                "totalChunks": total_chunks,
                "received": set(),
                "blob": Blob(size),
                "collectionToken": collection_token,
                "createdAt": time.time(),
            }
        return self.send_json(201, {
            "token": token,
            "chunkSize": CHUNK_SIZE,
            "totalChunks": total_chunks,
            "deleteToken": STATE["sessions"][token]["deleteToken"],
        })

    def upload_complete(self, token):
        session = STATE["sessions"].get(token)
        if session is None:
            return self.send_error_json(404, "Not found or expired.")
        if len(session["received"]) != session["totalChunks"]:
            return self.send_error_json(
                409, "Some chunks are still missing.",
                {"receivedChunks": len(session["received"]),
                 "totalChunks": session["totalChunks"]})

        collection_token = session["collectionToken"]
        if collection_token and collection_token in STATE["collections"]:
            expires = STATE["collections"][collection_token]["expiresAt"]
        else:
            expires = now_iso(retention_days(session["size"]))

        record = {
            "token": token,
            "filename": session["filename"],
            "mimeType": session["mimeType"],
            "size": session["size"],
            "blob": session["blob"],
            "deleteToken": session["deleteToken"],
            "uploadedAt": now_iso(),
            "expiresAt": expires,
            "collectionToken": collection_token,
        }
        with LOCK:
            STATE["files"][token] = record
            del STATE["sessions"][token]
            if collection_token and collection_token in STATE["collections"]:
                STATE["collections"][collection_token]["files"].append(token)

        return self.send_json(201, {
            "token": token,
            "url": "http://%s/d/%s" % (self.headers.get("Host", "localhost"),
                                       token),
            "filename": record["filename"],
            "size": record["size"],
            "mimeType": record["mimeType"],
            "uploadedAt": record["uploadedAt"],
            "expiresAt": record["expiresAt"],
            "deleteToken": record["deleteToken"],
            "collectionToken": collection_token,
        })

    def sharex(self):
        filename = self.headers.get("X-Filename") or "upload.bin"
        declared = int(self.headers.get("Content-Length") or 0)
        if declared > OPTS.max_upload:
            return self.send_error_json(
                413, "This mock server refuses uploads over %d bytes."
                     % OPTS.max_upload)
        data = self.read_body()
        blob = Blob()
        blob.append(data)
        token = new_token()
        record = {
            "token": token,
            "filename": filename,
            "mimeType": self.headers.get("Content-Type")
                        or "application/octet-stream",
            "size": len(data),
            "blob": blob,
            "deleteToken": new_token(24),
            "uploadedAt": now_iso(),
            "expiresAt": now_iso(retention_days(len(data))),
            "collectionToken": None,
        }
        with LOCK:
            STATE["files"][token] = record
        return self.send_json(201, {
            "token": token,
            "url": "http://%s/d/%s?inline=1"
                   % (self.headers.get("Host", "localhost"), token),
            "filename": record["filename"],
            "size": record["size"],
            "mimeType": record["mimeType"],
            "uploadedAt": record["uploadedAt"],
            "expiresAt": record["expiresAt"],
            "deleteToken": record["deleteToken"],
        })

    def collection_init(self):
        body = self.read_json()
        if body is None:
            return self.send_error_json(400, "Invalid JSON body.")
        title = body.get("title") or ""
        description = body.get("description") or ""
        if len(title) > 200:
            return self.send_error_json(400, "Title is too long.")
        if len(description) > 4000:
            return self.send_error_json(400, "Description is too long.")

        token = new_token()
        with LOCK:
            STATE["collections"][token] = {
                "token": token,
                "deleteToken": new_token(24),
                "title": title,
                "description": description,
                "files": [],
                "sealed": False,
                "uploadedAt": now_iso(),
                "expiresAt": now_iso(30),
            }
        record = STATE["collections"][token]
        return self.send_json(201, {
            "token": token,
            "deleteToken": record["deleteToken"],
            "expiresAt": record["expiresAt"],
        })

    def collection_complete(self, token):
        record = STATE["collections"].get(token)
        if record is None:
            return self.send_error_json(404, "Not found or expired.")
        if not record["files"]:
            return self.send_error_json(400, "This collection has no files.")
        record["sealed"] = True
        files = [STATE["files"][t] for t in record["files"]]
        return self.send_json(201, {
            "token": token,
            "type": "collection",
            "title": record["title"],
            "description": record["description"],
            "files": [{"token": f["token"], "filename": f["filename"],
                       "mimeType": f["mimeType"], "size": f["size"]}
                      for f in files],
            "fileCount": len(files),
            "totalSize": sum(f["size"] for f in files),
            "uploadedAt": record["uploadedAt"],
            "expiresAt": record["expiresAt"],
            "url": "http://%s/d/%s" % (self.headers.get("Host", "localhost"),
                                       token),
            "deleteToken": record["deleteToken"],
        })

    def uploads_exists(self):
        body = self.read_json()
        if body is None or not isinstance(body.get("tokens"), list):
            return self.send_error_json(400, "A list of tokens is required.")
        tokens = body["tokens"]
        if len(tokens) > 500:
            return self.send_error_json(400, "Too many tokens in one call.")
        existing = [t for t in tokens
                    if t in STATE["files"] or t in STATE["collections"]]
        return self.send_json(200, {"existing": existing})

    def reports(self):
        body = self.read_json()
        if body is None:
            return self.send_error_json(400, "Invalid JSON body.")
        link = body.get("link") or ""
        note = body.get("note") or ""
        if len(note) > 2000:
            return self.send_error_json(400, "That note is too long.")
        match = re.search(r"([A-Za-z0-9]{6,32})\s*$", link)
        if not match:
            return self.send_error_json(400, "That is not a zhuzhbox link.")
        token = match.group(1)
        if token not in STATE["files"] and token not in STATE["collections"]:
            return self.send_error_json(404, "Not found or expired.")
        if token in STATE["reports"]:
            return self.send_json(201, {"ok": True, "duplicate": True})
        STATE["reports"].add(token)
        return self.send_json(201, {"ok": True})

    def do_PUT(self):
        if self.should_drop():
            return self.drop()
        path = self.path.split("?", 1)[0]
        match = re.fullmatch(r"/v1/upload/([A-Za-z0-9]+)/chunk/(\d+)", path)
        if not match:
            return self.send_error_json(404, "Not found or expired.")

        token, index = match.group(1), int(match.group(2))
        session = STATE["sessions"].get(token)
        if session is None:
            return self.send_error_json(404, "Not found or expired.")
        if index >= session["totalChunks"]:
            return self.send_error_json(400, "Chunk index out of range.")

        offset = index * session["chunkSize"]
        expected = min(session["chunkSize"], session["size"] - offset)
        data = self.read_body()
        if len(data) != expected:
            return self.send_error_json(
                400, "Chunk %d must be exactly %d bytes, got %d."
                     % (index, expected, len(data)))

        if OPTS.chunk_delay:
            # Gives a test enough of a window to send SIGINT mid-upload.
            time.sleep(OPTS.chunk_delay)

        session["blob"].write_at(offset, data)
        with LOCK:
            session["received"].add(index)
        return self.send_json(200, {
            "receivedChunks": len(session["received"]),
            "totalChunks": session["totalChunks"],
        })

    def do_DELETE(self):
        if self.should_drop():
            return self.drop()
        path = self.path.split("?", 1)[0]
        match = re.fullmatch(r"/v1/upload/([A-Za-z0-9]+)", path)
        if not match:
            return self.send_error_json(404, "Not found or expired.")
        token = match.group(1)
        supplied = self.headers.get("X-Delete-Token")

        record = STATE["collections"].get(token)
        if record is not None:
            if supplied != record["deleteToken"]:
                return self.send_error_json(403, "That delete token is wrong.")
            with LOCK:
                for member in record["files"]:
                    gone = STATE["files"].pop(member, None)
                    if gone is not None:
                        gone["blob"].discard()
                del STATE["collections"][token]
            self.send_response(204)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        record = STATE["files"].get(token)
        if record is None:
            return self.send_error_json(404, "Not found or expired.")
        if supplied != record["deleteToken"]:
            return self.send_error_json(403, "That delete token is wrong.")
        with LOCK:
            del STATE["files"][token]
        record["blob"].discard()
        self.send_response(204)
        self.send_header("Content-Length", "0")
        self.end_headers()


def main():
    global OPTS, SPOOL_DIR
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--drop-rate", type=int, default=0)
    parser.add_argument("--capacity-once", action="store_true")
    parser.add_argument("--quota-exhausted", action="store_true")
    parser.add_argument("--ignore-range", action="store_true")
    parser.add_argument("--chunk-delay", type=float, default=0.0,
                        help="seconds to stall each chunk PUT")
    # A guard rail, not part of the API. Nothing in the suite needs more than a
    # few hundred megabytes, and an accidental multi-gigabyte upload here would
    # fill the spool directory (which is often a RAM-backed /tmp).
    parser.add_argument("--max-upload", type=int, default=512 * 1024 * 1024,
                        help="refuse uploads larger than this many bytes")
    # Letting the harness choose this puts the spooled payloads inside the
    # directory it already deletes, so they cannot survive even a SIGKILL.
    parser.add_argument("--spool-dir",
                        help="where to spool payloads (default: a temp dir)")
    parser.add_argument("--state-file")
    OPTS = parser.parse_args()

    if OPTS.spool_dir:
        os.makedirs(OPTS.spool_dir, exist_ok=True)
        SPOOL_DIR = OPTS.spool_dir
    else:
        SPOOL_DIR = tempfile.mkdtemp(prefix="zb-mock-spool-")

    # Test harnesses stop this process with SIGTERM, which would otherwise skip
    # the cleanup below and leave spooled payloads on disk.
    def on_terminate(signum, frame):
        del signum, frame
        raise SystemExit(0)

    signal.signal(signal.SIGTERM, on_terminate)
    atexit.register(shutil.rmtree, SPOOL_DIR, True)

    server = ThreadingHTTPServer(("127.0.0.1", OPTS.port), Handler)
    port = server.server_address[1]
    if OPTS.state_file:
        with open(OPTS.state_file, "w", encoding="utf-8") as handle:
            json.dump({"port": port}, handle)
    print(port, flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        # Never leave spooled payloads behind, however this process ends.
        shutil.rmtree(SPOOL_DIR, ignore_errors=True)


if __name__ == "__main__":
    main()
