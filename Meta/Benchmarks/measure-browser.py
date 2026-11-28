#!/usr/bin/env python3

# Copyright (c) 2026-present, the Ladybird developers.
# SPDX-License-Identifier: BSD-2-Clause

"""Run or resume a paired comparison of browser resources on Linux."""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

from pathlib import Path

from resource_trial import browser_config
from resource_trial import host_reservation


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True, help="JSON file describing the browser installations")
    parser.add_argument("--baseline", required=True, help="baseline label in the configuration")
    parser.add_argument("--candidate", required=True, help="candidate label in the configuration")
    parser.add_argument("--scenario", choices=["mixed", "reading"], default="mixed")
    parser.add_argument("--tabs", type=int, choices=[10, 25, 50], default=50)
    parser.add_argument("--runs", type=int, default=6, help="number of paired runs (default 6, minimum 5)")
    parser.add_argument(
        "--output", type=Path, help="new or resumable results directory (default: a temporary directory)"
    )
    args = parser.parse_args()
    if args.runs < 5 or args.baseline == args.candidate:
        parser.error("Supply at least five pairs and two distinct labels")

    kit = Path(__file__).resolve().parent
    controller = kit.parent / "measure-performance.py"
    config = args.config.resolve()
    inputs = [Path(__file__), config, kit / "resource_trial.py", kit / "fixture_server.py", kit / "workload.html"]
    try:
        for label in (args.baseline, args.candidate):
            _, executable, files = browser_config(config, label)
            inputs += [executable, *files]
        for path in [controller, *inputs]:
            if not path.is_file():
                parser.error(f"Missing input file: {path}")
        for name in ("timeout", "systemd-run", "systemctl", "xvfb-run", "Xvfb", "xauth", "xdotool"):
            if not shutil.which(name):
                parser.error(f"Missing required command: {name}")
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.error(f"Invalid benchmark configuration: {error}")

    output = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="ladybird-benchmark-"))
    print(f"Results directory: {output}", flush=True)
    command = [
        sys.executable,
        str(controller),
        "--output",
        str(output),
        "--baseline",
        args.baseline,
        "--candidate",
        args.candidate,
        "--runs",
        str(args.runs),
        "--seed",
        "27",
        "--batch-blocks",
        "2",
    ]
    for path in dict.fromkeys(path.resolve() for path in inputs):
        command.extend(["--input", str(path)])
    command += [
        "--",
        sys.executable,
        str(kit / "resource_trial.py"),
        "--config",
        str(config),
        "--scenario",
        args.scenario,
        "--tabs",
        str(args.tabs),
        "--variant",
        "{variant}",
        "--output",
        "{output}",
    ]
    while True:
        try:
            with host_reservation() as lock:
                result = subprocess.run(
                    ["timeout", "--kill-after=4s", "290s", *command],
                    cwd=kit,
                    stdin=lock,
                )
        except BlockingIOError:
            print("Host reservation busy. Resume with the same --output.", file=sys.stderr)
            return os.EX_TEMPFAIL
        if result.returncode:
            return result.returncode
        if json.loads((output / "summary.json").read_bytes())["status"] == "complete":
            return 0


if __name__ == "__main__":
    sys.exit(main())
