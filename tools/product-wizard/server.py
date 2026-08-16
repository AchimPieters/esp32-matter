#!/usr/bin/env python3
"""Local companion server for the product wizard — build/flash/package a
device straight from the wizard's Generate Firmware step, with a live
console and a live QR code, instead of only copy-paste commands.

Entirely optional and additive: the wizard (index.html) still works exactly
as before if you just double-click it (file://) — no server, no network
calls, pure copy-paste commands, same as every other device type's README
already documents. This script only adds an *alternative* way to reach the
same two commands (`docker run ... idf.py build ... gen_factory.sh` and
`esptool.py ... write_flash ...`) — same commands, same Docker image, same
host-side flashing constraint (Docker Desktop can't reach USB) documented
in CLAUDE.md — just triggered by a button instead of your own terminal.

Run it:

    cd tools/product-wizard
    python3 server.py

Then open the URL it prints (http://127.0.0.1:<port>/?token=<token>) instead
of double-clicking index.html. The token is generated fresh per run and only
ever printed to your own terminal + baked into the page this server itself
serves — it exists so a malicious page open in another browser tab can't
quietly poke this server into building/flashing something on your behalf
(the classic "any webpage can reach localhost" problem every local dev
server has to consider). Every request the API accepts must carry that same
token in an X-Wizard-Token header — no exceptions, no separate unauthenticated
paths into build/flash/package.

Security posture, spelled out plainly (this is a local dev tool, not a
hardened server — proportionate for that, not enterprise-grade):
  - Binds to 127.0.0.1 only. Never 0.0.0.0 — nothing outside this machine
    can ever reach it.
  - Every /api/* request must carry the per-run token (checked in
    _check_token()) and originate from this same server's own origin
    (checked via the Origin header in _check_origin()) — closes off both
    "guess the port" and "malicious page's fetch()" attack shapes.
  - Every parameter that ends up inside a shell command is validated
    against a fixed allow-list *on this side*, not trusted from the
    browser's own (already-careful) string-building in index.html —
    firmware directory, target chip, and bin name all come from
    FIRMWARE_DIRS/MODULES below, not from client-supplied strings; sed
    name/value pairs are regex-validated against the exact shapes
    buildSedCommandPairs() in index.html produces; the serial port is checked
    against the actual list of ports this machine currently reports
    (list_serial_ports()), not an arbitrary path. A request that doesn't
    match is rejected outright — never silently sanitized and run anyway.
"""

import glob
import http.server
import json
import os
import re
import secrets
import shutil
import socket
import subprocess
import sys
import threading
import time
import urllib.parse
import uuid
import zipfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, "..", ".."))

ESP_MATTER_IMAGE = "espressif/esp-matter:release-v1.6_idf_v5.5.4"

# Kept in sync BY HAND with DEVICE_TYPES/MODULES in index.html — a mismatch
# here only ever causes a clear "unknown device type"/"unknown target"
# rejection (fail-closed), never a validation hole, so this duplication is a
# deliberate simplicity-over-cleverness tradeoff rather than parsing
# index.html's own JS at startup to derive it automatically.
FIRMWARE_DIRS = {
    "light": ("firmware/light", "matter_light.bin"),
    "switch": ("firmware/switch", "matter_switch.bin"),
    "contact-sensor": ("firmware/contact-sensor", "matter_contact_sensor.bin"),
    "outlet": ("firmware/outlet", "matter_outlet.bin"),
    "temperature": ("firmware/temperature-sensor", "matter_temperature_sensor.bin"),
    "light-sensor": ("firmware/light-sensor", "matter_light_sensor.bin"),
    "dimmable-light": ("firmware/dimmable-light", "matter_dimmable_light.bin"),
    "window-covering": ("firmware/window-covering", "matter_window_covering.bin"),
    "color-light": ("firmware/color-light", "matter_color_light.bin"),
    "addressable-light": ("firmware/addressable-light", "matter_addressable_light.bin"),
    "thermostat": ("firmware/thermostat", "matter_thermostat.bin"),
    "door-lock": ("firmware/door-lock", "matter_door_lock.bin"),
    "smoke-co-alarm": ("firmware/smoke-co-alarm", "matter_smoke_co_alarm.bin"),
}

MODULES = {
    "esp32": "0x1000",
    "esp32c2": "0x0",
    "esp32c3": "0x0",
    "esp32c5": "0x0",
    "esp32c6": "0x0",
    "esp32c61": "0x0",
    "esp32s3": "0x0",
    "esp32h2": "0x0",
}

# Exactly the shapes buildSedCommands() in index.html ever produces for a
# #define's replacement value — a bare GPIO_NUM_<n>/GPIO_NUM_NC constant, a
# plain integer (numberField), or another ALL_CAPS enum-style constant
# (component/extraPicker defineValue, e.g. SENSOR_MQ2_MQ7). Anything else is
# rejected outright.
DEFINE_NAME_RE = re.compile(r"^[A-Z][A-Z0-9_]*$")
DEFINE_VALUE_RE = re.compile(r"^(GPIO_NUM_(NC|[0-9]{1,2})|-?[0-9]+|[A-Z][A-Z0-9_]*)$")
PRODUCT_NAME_RE = re.compile(r"^[a-zA-Z0-9 _-]{1,64}$")

TOKEN = secrets.token_urlsafe(24)


def list_serial_ports():
    """Best-effort serial port list. pyserial is the standard way to do this
    portably, but it's not a hard dependency of this repo elsewhere (only
    esptool.py itself needs it, transitively) — degrade to an empty list
    with a clear message rather than crashing the whole server if it's
    missing, since /api/ports is the only endpoint that needs it."""
    try:
        from serial.tools import list_ports  # type: ignore
    except ImportError:
        return None
    return sorted(p.device for p in list_ports.comports())


class Job:
    """One running (or finished) subprocess, tailed by one or more SSE
    readers. Output is kept as a plain list rather than a real pub/sub queue
    — simplest thing that works for a single-user local tool; readers just
    poll their own read index."""

    def __init__(self, job_id, label):
        self.id = job_id
        self.label = label
        self.lines = []
        self.lock = threading.Lock()
        self.status = "running"  # running | done | error
        self.returncode = None
        self.extra = {}  # e.g. {"qrcode_path": "..."} once genfactory finds one

    def append(self, line):
        with self.lock:
            self.lines.append(line)

    def snapshot(self, start=0):
        with self.lock:
            return list(self.lines[start:]), self.status, self.returncode


JOBS = {}
JOBS_LOCK = threading.Lock()


def new_job(label):
    job = Job(uuid.uuid4().hex, label)
    with JOBS_LOCK:
        JOBS[job.id] = job
    return job


def run_job(job, cmd, cwd, on_done=None):
    """Runs cmd (a list — never shell=True, so nothing built from validated
    parts above can be reinterpreted by a shell) in a background thread,
    streaming each output line into job.lines as it arrives."""

    def worker():
        try:
            proc = subprocess.Popen(
                cmd,
                cwd=cwd,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1,
            )
            for line in proc.stdout:
                job.append(line.rstrip("\n"))
            proc.wait()
            job.returncode = proc.returncode
            job.status = "done" if proc.returncode == 0 else "error"
        except Exception as exc:  # noqa: BLE001 — surface any failure into the console
            job.append(f"[server] Failed to run command: {exc}")
            job.status = "error"
        if on_done:
            try:
                on_done(job)
            except Exception as exc:  # noqa: BLE001
                job.append(f"[server] on_done callback failed: {exc}")

    threading.Thread(target=worker, daemon=True).start()


# Serial ports currently held open by a running monitor job, keyed by job
# id — lets /api/monitor/stop close the real handle rather than only
# setting a flag the read loop might not notice for a while.
SERIAL_HANDLES = {}
SERIAL_BAUD_RATES = {9600, 74880, 115200, 230400}  # same choices as the
# existing Web-Serial-based monitor's own <select> in index.html, kept in
# sync by hand for the same reason FIRMWARE_DIRS/MODULES above are.


def run_serial_monitor_job(job, port, baud):
    """Live serial reader — this is the Safari fallback for the wizard's
    existing Test Product step: browsers that don't implement the Web
    Serial API (Safari, and WebKit has stated it does not intend to —
    their own public standards-positions list marks it "negative") can't
    read a serial port from JavaScript at all, with or without an
    extension (a Safari Web Extension is still confined to the same
    extension API surface, which has no serial access; only a full,
    separately-signed Safari *App* Extension with a native-messaging-style
    helper process could do it — strictly more moving parts than just
    letting this already-running local server do the read itself, which
    works in literally any browser, not only Safari). Reuses the exact
    same generic /api/jobs/<id>/stream SSE endpoint every other job type
    already streams through — the only difference from run_job() above is
    that this reads from a live serial port instead of a subprocess, and
    keeps going indefinitely until /api/monitor/stop is called rather than
    finishing on its own."""

    def worker():
        try:
            import serial  # local import: only this one code path needs pyserial
        except ImportError:
            job.append("[server] pyserial is not installed — run: pip install pyserial")
            job.status = "error"
            return
        try:
            ser = serial.Serial(port, baudrate=baud, timeout=1)
        except Exception as exc:  # noqa: BLE001
            job.append(f"[server] Could not open {port}: {exc}")
            job.status = "error"
            return
        SERIAL_HANDLES[job.id] = ser
        try:
            while job.status == "running":
                try:
                    raw = ser.readline()  # blocks at most `timeout` seconds
                except Exception:
                    break  # port closed (Stop was clicked) or device unplugged
                if raw:
                    job.append(raw.decode("utf-8", errors="replace").rstrip("\r\n"))
        finally:
            SERIAL_HANDLES.pop(job.id, None)
            try:
                ser.close()
            except Exception:  # noqa: BLE001
                pass
            if job.status == "running":
                job.status = "done"

    threading.Thread(target=worker, daemon=True).start()


def docker_run_argv(inner_bash):
    return [
        "docker", "run", "--rm", "-v", f"{REPO_ROOT}:/project", "-w", "/project",
        ESP_MATTER_IMAGE, "bash", "-c", inner_bash,
    ]


def validate_device_type(body):
    device_type = body.get("deviceType")
    if device_type not in FIRMWARE_DIRS:
        raise ValueError(f"Unknown deviceType: {device_type!r}")
    return device_type


def validate_target(body):
    target = body.get("target")
    if target not in MODULES:
        raise ValueError(f"Unknown target: {target!r}")
    return target


def validate_sed_commands(body):
    out = []
    for item in body.get("sedCommands", []):
        name = item.get("name", "")
        value = item.get("value", "")
        if not DEFINE_NAME_RE.match(name):
            raise ValueError(f"Rejected defineName: {name!r}")
        if not DEFINE_VALUE_RE.match(value):
            raise ValueError(f"Rejected value for {name}: {value!r}")
        out.append((name, value))
    return out


def validate_product_name(body):
    name = body.get("productName", "Device")
    if not PRODUCT_NAME_RE.match(name):
        return "Device"
    return name


class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "esp32-matter-wizard/1.0"

    # --- small helpers ---------------------------------------------------
    def _send_json(self, obj, status=200):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self._send_cors_headers()
        self.end_headers()
        self.wfile.write(body)

    def _send_cors_headers(self):
        # Same-origin only in practice (see _check_origin), but browsers
        # still need an explicit Access-Control-Allow-Origin echoing this
        # server's own origin for fetch() from the page it itself served.
        origin = self.headers.get("Origin")
        if origin and self._origin_is_self(origin):
            self.send_header("Access-Control-Allow-Origin", origin)
            self.send_header("Access-Control-Allow-Headers", "X-Wizard-Token, Content-Type")

    def _origin_is_self(self, origin):
        try:
            parsed = urllib.parse.urlparse(origin)
            host, _, port = self.headers.get("Host", "").partition(":")
            return parsed.hostname in ("127.0.0.1", "localhost") and str(parsed.port) == (port or "80")
        except Exception:  # noqa: BLE001
            return False

    def _check_auth(self):
        """Every /api/* request must carry the per-run token AND, if an
        Origin header is present at all (fetch() always sends one for
        cross-origin-*capable* requests; same-origin navigations may omit
        it), that origin must be this same server. Rejects anything else
        with 403 before touching any request body."""
        origin = self.headers.get("Origin")
        if origin and not self._origin_is_self(origin):
            self._send_json({"error": "origin not allowed"}, 403)
            return False
        token = self.headers.get("X-Wizard-Token") or self.query.get("token", [None])[0]
        if token != TOKEN:
            self._send_json({"error": "missing or invalid token"}, 403)
            return False
        return True

    def _read_json_body(self):
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length) if length else b"{}"
        return json.loads(raw or b"{}")

    def log_message(self, fmt, *args):  # quieter default logging
        sys.stderr.write("[server] " + (fmt % args) + "\n")

    # --- routing -----------------------------------------------------------
    def do_OPTIONS(self):
        self.send_response(204)
        self._send_cors_headers()
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.end_headers()

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        self.query = urllib.parse.parse_qs(parsed.query)
        path = parsed.path

        if path == "/" or path == "/index.html":
            self._serve_index()
            return
        if path == "/api/health":
            if not self._check_auth():
                return
            self._send_json({"ok": True, "repoRoot": REPO_ROOT})
            return
        if path == "/api/ports":
            if not self._check_auth():
                return
            ports = list_serial_ports()
            self._send_json({"ports": ports if ports is not None else [],
                              "pyserialMissing": ports is None})
            return
        if path.startswith("/api/jobs/") and path.endswith("/stream"):
            if not self._check_auth():
                return
            job_id = path.split("/")[3]
            self._stream_job(job_id)
            return
        if path.startswith("/api/download/"):
            if not self._check_auth():
                return
            self._serve_download(path.split("/")[3])
            return
        if path.startswith("/api/qrcode/"):
            if not self._check_auth():
                return
            self._serve_qrcode(path.split("/")[3])
            return
        self._send_json({"error": "not found"}, 404)

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        self.query = urllib.parse.parse_qs(parsed.query)
        path = parsed.path
        if not self._check_auth():
            return
        try:
            body = self._read_json_body()
        except Exception:  # noqa: BLE001
            self._send_json({"error": "invalid JSON body"}, 400)
            return

        try:
            if path == "/api/build":
                self._start_build(body)
            elif path == "/api/flash":
                self._start_flash(body)
            elif path == "/api/package":
                self._start_package(body)
            elif path == "/api/monitor/start":
                self._start_monitor(body)
            elif path == "/api/monitor/stop":
                self._stop_monitor(body)
            else:
                self._send_json({"error": "not found"}, 404)
        except ValueError as exc:
            self._send_json({"error": str(exc)}, 400)

    # --- static page serving ------------------------------------------------
    def _serve_index(self):
        index_path = os.path.join(SCRIPT_DIR, "index.html")
        with open(index_path, "r", encoding="utf-8") as f:
            html = f.read()
        # Injects the per-run token as a global JS var — index.html checks
        # for this to decide whether to show the interactive Build & Flash
        # UI at all. Opening the plain file:// copy never defines this, so
        # that mode is completely unaffected.
        injected = f"<script>window.__WIZARD_TOKEN__ = {json.dumps(TOKEN)};</script>\n</head>"
        html = html.replace("</head>", injected, 1)
        body = html.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    # --- SSE job streaming ---------------------------------------------------
    def _stream_job(self, job_id):
        job = JOBS.get(job_id)
        if not job:
            self._send_json({"error": "unknown job"}, 404)
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self._send_cors_headers()
        self.end_headers()

        sent = 0
        try:
            while True:
                lines, status, returncode = job.snapshot(sent)
                for line in lines:
                    payload = json.dumps({"line": line})
                    self.wfile.write(f"data: {payload}\n\n".encode("utf-8"))
                sent += len(lines)
                if status != "running" and sent >= len(job.lines):
                    done_payload = json.dumps({
                        "status": status,
                        "returncode": job.returncode,
                        "extra": job.extra,
                    })
                    self.wfile.write(f"event: done\ndata: {done_payload}\n\n".encode("utf-8"))
                    self.wfile.flush()
                    break
                self.wfile.flush()
                time.sleep(0.15)
        except (BrokenPipeError, ConnectionResetError):
            pass  # client navigated away — nothing to clean up, the job keeps running

    # --- /api/build: sed + idf.py build + gen_factory.sh, all in Docker ----
    def _start_build(self, body):
        device_type = validate_device_type(body)
        target = validate_target(body)
        sed_pairs = validate_sed_commands(body)
        product_name = validate_product_name(body)
        firmware_dir, _bin_name = FIRMWARE_DIRS[device_type]

        sed_cmds = [
            f"sed -i 's/^#define {name} .*/#define {name} {value}/' main/app_main.cpp"
            for name, value in sed_pairs
        ]
        inner = " && ".join([
            f"cd /project/{firmware_dir}",
            *sed_cmds,
            f"idf.py set-target {target}",
            "idf.py build",
            f"PRODUCT_NAME='{product_name}' /project/tools/gen_factory.sh",
        ])

        job = new_job("build")

        def on_done(finished_job):
            # Find the QR code gen_factory.sh just produced, if the build
            # got that far — served later via /api/qrcode/<job_id>.
            pattern = os.path.join(REPO_ROOT, firmware_dir, "out", "*", "*", "*-qrcode.png")
            matches = glob.glob(pattern)
            if matches:
                finished_job.extra["qrcode_path"] = max(matches, key=os.path.getmtime)

        run_job(job, docker_run_argv(inner), REPO_ROOT, on_done=on_done)
        self._send_json({"jobId": job.id})

    # --- /api/flash: esptool.py write_flash, run directly on the HOST ------
    # (never inside Docker — Docker Desktop can't reach USB, see CLAUDE.md).
    def _start_flash(self, body):
        device_type = validate_device_type(body)
        target = validate_target(body)
        firmware_dir, bin_name = FIRMWARE_DIRS[device_type]
        bootloader_offset = MODULES[target]

        port = body.get("port")
        available_ports = list_serial_ports() or []
        if port not in available_ports:
            raise ValueError(f"Port {port!r} is not in the currently detected port list {available_ports}")

        build_dir = os.path.join(REPO_ROOT, firmware_dir, "build")
        factory_matches = glob.glob(os.path.join(REPO_ROOT, firmware_dir, "out", "*", "*", "*-partition.bin"))
        if not factory_matches:
            raise ValueError("No factory partition found — run Build first (it also runs gen_factory.sh).")
        if len(factory_matches) > 1:
            raise ValueError(
                f"Found {len(factory_matches)} factory partitions under {firmware_dir}/out/ — "
                f"remove old runs (rm -rf {firmware_dir}/out) and Build again so exactly one matches."
            )
        factory_bin = factory_matches[0]

        cmd = [
            "esptool.py", "--chip", target, "-p", port, "-b", "460800",
            "--before", "default_reset", "--after", "hard_reset", "write_flash",
            "--flash_mode", "dio", "--flash_size", "4MB", "--flash_freq", "40m",
            bootloader_offset, os.path.join(build_dir, "bootloader", "bootloader.bin"),
            "0x8000", os.path.join(build_dir, "partition_table", "partition-table.bin"),
            "0x10000", os.path.join(build_dir, "ota_data_initial.bin"),
            "0x20000", os.path.join(build_dir, bin_name),
            "0x3E0000", factory_bin,
        ]

        job = new_job("flash")
        run_job(job, cmd, REPO_ROOT)
        self._send_json({"jobId": job.id})

    # --- /api/monitor/*: live serial log — the Safari fallback for the
    # wizard's existing Web-Serial-based Test Product monitor (see
    # run_serial_monitor_job()'s own comment for why this exists at all,
    # not just in Chrome/Edge). ---
    def _start_monitor(self, body):
        port = body.get("port")
        available_ports = list_serial_ports() or []
        if port not in available_ports:
            raise ValueError(f"Port {port!r} is not in the currently detected port list {available_ports}")
        baud = body.get("baud")
        if baud not in SERIAL_BAUD_RATES:
            raise ValueError(f"Unsupported baud rate: {baud!r}")

        job = new_job("monitor")
        run_serial_monitor_job(job, port, baud)
        self._send_json({"jobId": job.id})

    def _stop_monitor(self, body):
        job_id = body.get("jobId")
        job = JOBS.get(job_id)
        if not job:
            raise ValueError("Unknown monitor jobId")
        job.status = "done"
        ser = SERIAL_HANDLES.get(job_id)
        if ser is not None:
            try:
                ser.close()  # unblocks the read loop immediately instead of waiting out its 1s timeout
            except Exception:  # noqa: BLE001
                pass
        self._send_json({"ok": True})

    # --- /api/package: zip the already-built .bin files + a flash script ---
    def _start_package(self, body):
        device_type = validate_device_type(body)
        target = validate_target(body)
        firmware_dir, bin_name = FIRMWARE_DIRS[device_type]
        bootloader_offset = MODULES[target]
        build_dir = os.path.join(REPO_ROOT, firmware_dir, "build")

        required = {
            "bootloader.bin": os.path.join(build_dir, "bootloader", "bootloader.bin"),
            "partition-table.bin": os.path.join(build_dir, "partition_table", "partition-table.bin"),
            "ota_data_initial.bin": os.path.join(build_dir, "ota_data_initial.bin"),
            bin_name: os.path.join(build_dir, bin_name),
        }
        missing = [name for name, path in required.items() if not os.path.isfile(path)]
        if missing:
            raise ValueError(f"Missing build output: {', '.join(missing)} — run Build first.")

        job = new_job("package")
        job.status = "running"

        def worker():
            try:
                out_dir = os.path.join(SCRIPT_DIR, "_packages")
                os.makedirs(out_dir, exist_ok=True)
                zip_name = f"{device_type}-{target}-{job.id[:8]}.zip"
                zip_path = os.path.join(out_dir, zip_name)

                flash_script = f"""#!/usr/bin/env bash
# Flashes this package to a connected {target} board. Usage:
#   ./flash.sh <PORT>
set -euo pipefail
PORT="${{1:?Usage: ./flash.sh <PORT>}}"
esptool.py --chip {target} -p "$PORT" -b 460800 \\
  --before default_reset --after hard_reset write_flash \\
  --flash_mode dio --flash_size 4MB --flash_freq 40m \\
  {bootloader_offset} bootloader.bin \\
  0x8000 partition-table.bin \\
  0x10000 ota_data_initial.bin \\
  0x20000 {bin_name}
echo "Flashed. This package does not include a factory/QR partition —"
echo "pair it into your fabric separately with your own tools/gen_factory.sh output."
"""
                with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
                    for arcname, path in required.items():
                        zf.write(path, arcname)
                    zf.writestr("flash.sh", flash_script)
                    zf.writestr(
                        "README.txt",
                        f"esp32-matter — {device_type} for {target}\n"
                        f"Generated by tools/product-wizard/server.py.\n\n"
                        f"To flash: chmod +x flash.sh && ./flash.sh <PORT>\n\n"
                        f"This package intentionally does NOT include a factory/QR "
                        f"partition (that's per-device commissioning data, generated "
                        f"fresh by tools/gen_factory.sh) — flash this app image, then "
                        f"generate + flash factory data separately for each physical "
                        f"unit you build.\n",
                    )
                job.extra["zip_name"] = zip_name
                job.append(f"[server] Package written: {zip_name}")
                job.status = "done"
                job.returncode = 0
            except Exception as exc:  # noqa: BLE001
                job.append(f"[server] Packaging failed: {exc}")
                job.status = "error"

        threading.Thread(target=worker, daemon=True).start()
        self._send_json({"jobId": job.id})

    # --- file serving for the two binary outputs ---------------------------
    def _serve_qrcode(self, job_id):
        job = JOBS.get(job_id)
        path = job.extra.get("qrcode_path") if job else None
        if not path or not os.path.isfile(path):
            self._send_json({"error": "no qrcode for this job (yet)"}, 404)
            return
        with open(path, "rb") as f:
            data = f.read()
        self.send_response(200)
        self.send_header("Content-Type", "image/png")
        self.send_header("Content-Length", str(len(data)))
        self._send_cors_headers()
        self.end_headers()
        self.wfile.write(data)

    def _serve_download(self, job_id):
        job = JOBS.get(job_id)
        zip_name = job.extra.get("zip_name") if job else None
        if not zip_name:
            self._send_json({"error": "no package for this job"}, 404)
            return
        path = os.path.join(SCRIPT_DIR, "_packages", zip_name)
        if not os.path.isfile(path):
            self._send_json({"error": "package file missing"}, 404)
            return
        with open(path, "rb") as f:
            data = f.read()
        self.send_response(200)
        self.send_header("Content-Type", "application/zip")
        self.send_header("Content-Disposition", f'attachment; filename="{zip_name}"')
        self.send_header("Content-Length", str(len(data)))
        self._send_cors_headers()
        self.end_headers()
        self.wfile.write(data)


def main():
    if shutil.which("docker") is None:
        print("WARNING: 'docker' not found on PATH — Build will fail until it is.", file=sys.stderr)
    if shutil.which("esptool.py") is None:
        print("WARNING: 'esptool.py' not found on PATH — Flash will fail until it is.", file=sys.stderr)

    port = 8787
    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler)
    url = f"http://127.0.0.1:{port}/?token={TOKEN}"
    print("esp32-matter product wizard — companion server")
    print(f"  Open: {url}")
    print("  (Ctrl-C to stop. Nothing here is reachable outside this machine.)")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
