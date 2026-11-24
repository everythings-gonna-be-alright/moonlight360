#!/usr/bin/env python3
"""Dev server for devlaunch.xex: serves the current build of a program to the console and takes its logs back.

    python3 dev/devserver.py [--port 7380] [--target moonlight360]

The target can be changed while it runs by writing its name to build/devserver/target.txt.

  GET  /manifest       what the launcher should fetch and run (plain text, see manifest())
  GET  /files/<name>   a file named in the manifest
  POST /logs/<name>    a log from the console, saved to build/devserver/logs/<time>-<name>
"""
import argparse
import datetime
import http.server
import os
import socketserver
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
STATE = ROOT / "build" / "devserver"
LOGS = STATE / "logs"
TARGET_FILE = STATE / "target.txt"
# A number the launcher compares with the last one it ran: bumping it (dev/devserver.py --bump, or writing the
# file) makes a waiting launcher download and run the target by itself.
RUN_FILE = STATE / "run.txt"
# Test arguments passed to the launched program (e.g. "autotest seconds=120 draw=1"); empty for a normal start.
ARGS_FILE = STATE / "args.txt"

# Each target: the .xex the launcher runs, and the files it downloads (served name -> local path, or None for a
# file the server makes per request). The launcher never overwrites files not listed here (moonlight.ini, keys).
TARGETS = {
    # The launcher updating itself: it arrives as devlaunch_new.xex, which copies itself over devlaunch.xex.
    "devlaunch": {
        "launch": "devlaunch_new.xex",
        "files": {"devlaunch_new.xex": ROOT / "build/devlaunch/dist/devlaunch.xex"},
    },
    "decbench": {
        "launch": "decbench.xex",
        # decbench (FFmpeg in C) runs first and starts decbench_vmx (FFmpeg's AltiVec code) when done.
        "files": {"decbench.xex": ROOT / "build/bench-xsdk/decbench.xex",
                  "decbench_vmx.xex": ROOT / "build/bench-xsdk/decbench_vmx.xex"},
    },
    # The AltiVec decode benchmark alone (its chroma-in-C passes give a before and after in one run).
    "decbench_vmx": {
        "launch": "decbench_vmx.xex",
        "files": {"decbench_vmx.xex": ROOT / "build/bench-xsdk/decbench_vmx.xex"},
    },
    "moonlight360": {
        "launch": "moonlight360.xex",
        "files": {"moonlight360.xex": ROOT / "build/app/dist/moonlight360.xex"},
    },
}


def current_run():
    try:
        return int(RUN_FILE.read_text().strip() or 0)
    except (OSError, ValueError):
        return 0


def current_target(default):
    try:
        name = TARGET_FILE.read_text().strip()
        if name in TARGETS:
            return name
    except OSError:
        pass
    return default


class Handler(http.server.BaseHTTPRequestHandler):
    default_target = "moonlight360"
    def log_message(self, fmt, *args):
        print(f"{datetime.datetime.now():%H:%M:%S} {self.client_address[0]} {fmt % args}", flush=True)

    def generated(self, name):
        """A file the server makes per request (a target's files entry of None); none at the moment."""
        return b""

    def file_bytes(self, target, name):
        path = TARGETS[target]["files"].get(name)
        return path.read_bytes() if path else self.generated(name)

    @staticmethod
    def owner(name):
        """Any target serving this file: a launcher holding an older manifest still gets what it asks for."""
        for target, spec in TARGETS.items():
            if name in spec["files"]:
                return target
        return None

    def reply(self, body, status=200, kind="text/plain"):
        self.send_response(status)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        target = current_target(self.default_target)
        if self.path == "/manifest":
            lines = [f"target {target}", f"run {current_run()}"]
            try:
                args = ARGS_FILE.read_text().strip()
            except OSError:
                args = ""
            if args:
                lines.append(f"args {args}")
            for name in TARGETS[target]["files"]:
                data = self.file_bytes(target, name)
                lines.append(f"file {name} {len(data)}")
            lines.append(f"launch {TARGETS[target]['launch']}")
            return self.reply(("\n".join(lines) + "\n").encode())
        if self.path.startswith("/files/"):
            name = self.path[len("/files/"):]
            owner = target if name in TARGETS[target]["files"] else self.owner(name)
            if owner is None:
                return self.reply(b"no such file\n", 404)
            return self.reply(self.file_bytes(owner, name), kind="application/octet-stream")
        self.reply(b"not found\n", 404)

    def do_POST(self):
        if not self.path.startswith("/logs/"):
            return self.reply(b"not found\n", 404)
        name = os.path.basename(self.path[len("/logs/"):]) or "unnamed.log"
        length = int(self.headers.get("Content-Length", "0"))
        data = self.rfile.read(length)
        LOGS.mkdir(parents=True, exist_ok=True)
        out = LOGS / f"{datetime.datetime.now():%Y%m%d-%H%M%S}-{name}"
        out.write_bytes(data)
        print(f"saved log {out} ({len(data)} bytes)", flush=True)
        self.reply(b"ok\n")


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=7380)
    ap.add_argument("--target", default="moonlight360", choices=sorted(TARGETS))
    ap.add_argument("--probe-port", type=int, default=7400)
    ap.add_argument("--bump", action="store_true", help="ask waiting launchers to run the target again, then exit")
    ap.add_argument("--set-target", choices=sorted(TARGETS), help="change the target (with --bump: and run it)")
    ap.add_argument("--args", help='test arguments for the next runs ("" for none)')
    args = ap.parse_args()
    if args.set_target or args.bump or args.args is not None:
        STATE.mkdir(parents=True, exist_ok=True)
        if args.args is not None:
            ARGS_FILE.write_text(args.args + "\n")
        if args.set_target:
            TARGET_FILE.write_text(args.set_target + "\n")
        if args.bump:
            RUN_FILE.write_text(f"{current_run() + 1}\n")
        print(f"target {current_target(args.target)}, run {current_run()}, args "
              f"{ARGS_FILE.read_text().strip() if ARGS_FILE.exists() else ''!r}")
        return
    Handler.default_target = args.target
    Handler.probe_port = args.probe_port
    STATE.mkdir(parents=True, exist_ok=True)
    print(f"devserver on port {args.port}, target {current_target(args.target)} "
          f"(change it in {TARGET_FILE})", flush=True)
    Server(("", args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
