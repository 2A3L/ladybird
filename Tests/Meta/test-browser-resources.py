# Copyright (c) 2026-present, the Ladybird developers.
# SPDX-License-Identifier: BSD-2-Clause

"""Check the local page protocol and resource trial acceptance without a browser."""

import importlib
import json
import os
import subprocess
import sys
import tempfile
import unittest

from pathlib import Path
from unittest.mock import patch
from urllib.parse import urlsplit
from urllib.request import ProxyHandler
from urllib.request import build_opener

KIT = Path(__file__).resolve().parents[2] / "Meta/Benchmarks"
sys.path.insert(0, str(KIT))

fixture = importlib.import_module("fixture_server")
resources = importlib.import_module("resource_trial")


class ResourceTrialTest(unittest.TestCase):
    def test_script_requires_a_native_browser_fingerprint(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            launcher = root / "launch"
            launcher.write_text("#!/bin/sh\n")
            launcher.chmod(0o700)
            config = root / "browsers.json"
            entry = {"browser": "Firefox", "executable": "launch", "fingerprint": ["launch"]}
            config.write_text(json.dumps({"label": entry}))
            with self.assertRaisesRegex(ValueError, "native browser executable"):
                resources.browser_config(config, "label")
            entry["fingerprint"] = [sys.executable]
            config.write_text(json.dumps({"label": entry}))
            _, actual, files = resources.browser_config(config, "label")
            self.assertEqual((actual, files), (launcher, [Path(sys.executable).resolve()]))

    def test_child_keeps_reservation_after_launcher_releases_it(self):
        with tempfile.TemporaryDirectory() as tmp, patch.dict(os.environ, XDG_RUNTIME_DIR=tmp):
            with resources.host_reservation() as lock:
                # Keep stdin open until the parent signals over a separate pipe.
                release, signal = os.pipe()
                child = subprocess.Popen(
                    [
                        sys.executable,
                        "-c",
                        "import os,sys; from resource_trial import inherited_reservation; "
                        "print(inherited_reservation(), flush=True); os.read(int(sys.argv[1]), 1)",
                        str(release),
                    ],
                    cwd=KIT,
                    stdin=lock,
                    stdout=subprocess.PIPE,
                    text=True,
                    pass_fds=(release,),
                )
                os.close(release)
            try:
                assert child.stdout is not None
                self.assertEqual(child.stdout.readline().strip(), "True")
                with self.assertRaises(BlockingIOError), resources.host_reservation():
                    self.fail("Released while still held by the child")
            finally:
                os.close(signal)
                child.wait(timeout=3)
                if child.stdout:
                    child.stdout.close()
            with resources.host_reservation():
                pass

    def test_busy_host_does_not_start_an_attempt(self):
        with tempfile.TemporaryDirectory() as tmp, patch.dict(os.environ, XDG_RUNTIME_DIR=tmp):
            root = Path(tmp)
            entry = {"browser": "Ladybird", "executable": sys.executable}
            config = root / "browsers.json"
            config.write_text(json.dumps({"a": entry, "b": entry}))
            output = root / "output"
            # Only the controller preflight runs. The test does not require X11 or a systemd user session.
            for name in ("timeout", "systemd-run", "systemctl", "xvfb-run", "Xvfb", "xauth", "xdotool"):
                (root / name).symlink_to("/bin/false")
            with resources.host_reservation():
                result = subprocess.run(
                    [
                        sys.executable,
                        str(KIT / "measure-browser.py"),
                        "--config",
                        str(config),
                        "--baseline",
                        "a",
                        "--candidate",
                        "b",
                        "--output",
                        str(output),
                    ],
                    capture_output=True,
                    env={**os.environ, "PATH": tmp},
                )
            self.assertEqual(result.returncode, os.EX_TEMPFAIL, result.stderr)
            self.assertFalse(output.exists())

    def test_reading_receipts(self):
        trial = fixture.Trial("reading-test", "reading", 10)
        client = build_opener(ProxyHandler({}))
        with fixture.serve_pages(trial) as urls:

            def get(url):
                with client.open(url, timeout=2) as response:
                    return response.read()

            for url in urls:
                self.assertIn(b"Tab resource lab", get(url))
                parts = urlsplit(url)
                get(f"{parts.scheme}://{parts.netloc}/event?{parts.query}&event=loaded&count=220")
            self.assertTrue(trial.pages_ready())
            trial.start_phase()
            parts = urlsplit(urls[-1])
            event = f"{parts.scheme}://{parts.netloc}/event?{parts.query}"
            get(event + "&event=done&generation=1&completed=120&offset=1920")
            self.assertEqual(len(trial.receipts()), 1)
            get(event + "&event=loaded&count=220")
            with self.assertRaisesRegex(ValueError, "reloaded"):
                trial.receipts()

    def test_mixed_requires_all_dashboard_receipts(self):
        trial = fixture.Trial("mixed-test", "mixed", 50)
        self.assertEqual(len(trial.targets), 27)
        for tab in trial.tabs:
            trial.events[(tab, "loaded")].append({"count": 240 if tab in trial.targets else 220})
        for generation in (1, 2):
            trial.start_phase()
            for tab in trial.targets:
                self.assertFalse(trial.receipts())
                trial.events[(tab, "done")].append({"generation": generation, "completed": 6})
            self.assertEqual(len(trial.receipts()), 27)
        trial.events[(1, "done")][-1]["completed"] = 5
        with self.assertRaisesRegex(ValueError, "Incomplete workload"):
            trial.receipts()


if __name__ == "__main__":
    unittest.main()
