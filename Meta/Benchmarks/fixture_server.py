# Copyright (c) 2026-present, the Ladybird developers.
# SPDX-License-Identifier: BSD-2-Clause

"""Serve local benchmark pages and collect receipts for each phase of work."""

import contextlib
import http.server
import json
import threading
import time

from collections import defaultdict
from functools import partial
from pathlib import Path
from urllib.parse import parse_qs
from urllib.parse import urlencode
from urllib.parse import urlsplit

WORKLOAD = Path(__file__).with_name("workload.html").read_bytes()


class Trial:
    def __init__(self, token, scenario, tab_count):
        self.token = token
        self.tabs = range(1, tab_count + 1)
        self.dashboards = [tab for tab in self.tabs if scenario == "mixed" and (tab % 2 == 1 or tab in (10, 25, 50))]
        self.targets = self.dashboards or [tab_count]
        self.events = defaultdict(list)
        self.condition = threading.Condition()
        self.generation = 0
        self.closed = False
        self.expiry_ms = 0

    def start_phase(self):
        with self.condition:
            self.generation += 1
            self.expiry_ms = round((time.time() + 10) * 1000)
            self.condition.notify_all()

    def close(self):
        with self.condition:
            self.closed = True
            self.condition.notify_all()

    def pages_ready(self):
        with self.condition:
            for tab in self.tabs:
                if errors := self.events.get((tab, "error")):
                    raise ValueError(f"Tab {tab} reported {errors[0]}")
                loaded = self.events.get((tab, "loaded"), [])
                if len(loaded) > 1:
                    raise ValueError(f"Tab {tab} reloaded")
                if loaded and int(loaded[0]["count"]) != (240 if tab in self.dashboards else 220):
                    raise ValueError(f"Tab {tab} has incomplete content")
            return all((tab, "loaded") in self.events for tab in self.tabs)

    def receipts(self):
        if not self.pages_ready():
            return []
        with self.condition:
            receipts = []
            for tab in self.targets:
                done = [e for e in self.events.get((tab, "done"), []) if int(e["generation"]) == self.generation]
                if len(done) > 1:
                    raise ValueError(f"Duplicate completion from tab {tab}")
                if done:
                    receipt = done[0]
                    if int(receipt["completed"]) != (6 if self.dashboards else 120):
                        raise ValueError(f"Incomplete workload on tab {tab}")
                    if not self.dashboards and int(receipt["offset"]) != 1920:
                        raise ValueError(f"Incorrect scroll offset on tab {tab}")
                    receipts.append(receipt)
            return receipts if len(receipts) == len(self.targets) else []


class Handler(http.server.BaseHTTPRequestHandler):
    def __init__(self, *args, trial, **kwargs):
        self.trial = trial
        super().__init__(*args, **kwargs)

    def log_message(self, format, *args):
        pass

    def respond(self, status, payload=b"", kind="text/plain"):
        self.send_response(status)
        self.send_header("Content-Type", kind)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        with contextlib.suppress(BrokenPipeError, ConnectionResetError):
            self.wfile.write(payload)

    def do_GET(self):
        request = urlsplit(self.path)
        query = {k: v[0] for k, v in parse_qs(request.query).items()}
        state = self.trial
        if query.get("id") != state.token:
            self.respond(404)
            return
        try:
            tab = int(query["tab"])
            if tab not in state.tabs:
                raise ValueError
            if request.path == "/":
                self.respond(200, WORKLOAD, "text/html; charset=utf-8")
                return
            if request.path == "/event" and query["event"] in ("loaded", "done", "error", "visible"):
                with state.condition:
                    state.events[(tab, query["event"])].append(query)
                self.respond(204)
                return
            if request.path == "/job" and tab in state.targets:
                previous = int(query["generation"])
                with state.condition:
                    state.condition.wait_for(lambda: state.closed or state.generation > previous, timeout=35)
                    job = {"generation": state.generation, "expiry_ms": state.expiry_ms}
                    available = not state.closed and state.generation > previous
                if available:
                    self.respond(200, json.dumps(job).encode(), "application/json")
                else:
                    self.respond(204)
                return
        except (KeyError, ValueError):
            self.respond(400)
            return
        self.respond(404)


@contextlib.contextmanager
def serve_pages(trial):
    hosts = ("127.0.0.1", "127.0.0.2", "127.0.0.3")
    ports = []
    with contextlib.ExitStack() as resources:
        for _ in range(4 if trial.dashboards else 1):
            port = 0
            for host in hosts:
                server = resources.enter_context(
                    http.server.ThreadingHTTPServer((host, port), partial(Handler, trial=trial))
                )
                port = server.server_port
                threading.Thread(target=server.serve_forever, args=(0.1,), daemon=True).start()
                resources.callback(server.shutdown)
            ports.append(port)
        resources.callback(trial.close)
        urls = []
        for tab in trial.tabs:
            host = hosts[(tab - 1) % len(hosts)]
            port = ports[(tab - 1) // len(hosts) % len(ports)]
            query = urlencode(
                {
                    "id": trial.token,
                    "tab": tab,
                    "kind": "dashboard" if tab in trial.dashboards else "article",
                    "scroll": int(not trial.dashboards and tab in trial.targets),
                }
            )
            urls.append(f"http://{host}:{port}/?{query}")
        yield urls
