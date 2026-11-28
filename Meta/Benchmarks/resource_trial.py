#!/usr/bin/env python3

# Copyright (c) 2026-present, the Ladybird developers.
# SPDX-License-Identifier: BSD-2-Clause

"""Run one browser trial under Linux cgroup v2 on a private Xvfb display."""

import argparse
import contextlib
import fcntl
import json
import os
import re
import secrets
import signal
import statistics
import subprocess
import sys
import tempfile
import time

from pathlib import Path

from fixture_server import Trial
from fixture_server import serve_pages

BROWSERS = ("Ladybird", "Firefox", "Chromium", "Chrome", "Edge", "Vivaldi")
LIMITS = ["--property=CPUQuota=200%", "--property=MemoryMax=2G"]
CGROUP_ROOT = Path("/sys/fs/cgroup")


def lock_path():
    return Path(os.environ.get("XDG_RUNTIME_DIR") or "/tmp") / f"agent-performance-{os.getuid()}.lock"


@contextlib.contextmanager
def host_reservation():
    with lock_path().open("a+") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield lock


def inherited_reservation():
    try:
        if not os.path.samefile(0, lock_path()):
            return False
    except FileNotFoundError:
        return False
    status = Path("/proc/self/fdinfo/0").read_text()
    return bool(re.search(r"^lock:\s+\d+: FLOCK\s+ADVISORY\s+WRITE\s+", status, re.MULTILINE))


def is_elf_executable(path):
    if not path.is_file() or not os.access(path, os.X_OK):
        return False
    with path.open("rb") as stream:
        header = stream.read(32)
    if len(header) != 32 or header[:4] != b"\x7fELF" or header[4] not in (1, 2) or header[5] not in (1, 2):
        return False
    # Executables (including PIE) have a program entry point. Shared libraries normally have none.
    order = "little" if header[5] == 1 else "big"
    entry = header[24 : 28 if header[4] == 1 else 32]
    return int.from_bytes(header[16:18], order) in (2, 3) and any(entry)


def browser_config(path, label):
    entry = json.loads(path.read_bytes())[label]
    browser = entry["browser"]
    if browser not in BROWSERS:
        raise ValueError(f"Unsupported browser {browser!r}, expected one of {BROWSERS}")
    executable = (path.parent / entry["executable"]).resolve(strict=True)
    if not executable.is_file() or not os.access(executable, os.X_OK):
        raise ValueError(f"Browser is not executable: {executable}")
    files = [(path.parent / p).resolve(strict=True) for p in entry.get("fingerprint", [])]
    if not any(is_elf_executable(file) for file in [executable, *files]):
        raise ValueError(f"Include the native browser executable in fingerprint for launcher {executable}")
    return browser, executable, files


def systemctl(*args, **kwargs):
    return subprocess.run(["systemctl", "--user", *args], check=True, **kwargs)


def cgroup(unit):
    path = systemctl("show", "-p", "ControlGroup", "--value", unit, capture_output=True, text=True).stdout.strip()
    if not path.startswith("/") or path == "/":
        raise RuntimeError(f"No cgroup for {unit}")
    return CGROUP_ROOT / path[1:]


def verify_limits(group):
    quota, period = (group / "cpu.max").read_text().split()
    if quota == "max" or int(quota) != int(period) * 2 or (group / "memory.max").read_text().strip() != str(2**31):
        raise RuntimeError(f"Unexpected CPU or memory limit in {group}")


def fields(path):
    return {key: int(value) for key, value in (line.split() for line in path.read_text().splitlines())}


def pressure(path):
    return {line.split()[0]: int(line.split()[-1].split("=")[1]) for line in path.read_text().splitlines()}


def host_sample():
    memory = Path("/proc/meminfo").read_text().splitlines()
    vm = fields(Path("/proc/vmstat"))
    return {
        "t": time.monotonic(),
        "load": os.getloadavg(),
        "cpu_ticks": list(map(int, Path("/proc/stat").read_text().splitlines()[0].split()[1:])),
        "available_kib": int(next(line.split()[1] for line in memory if line.startswith("MemAvailable:"))),
        "swap_in_pages": vm["pswpin"],
        "swap_out_pages": vm["pswpout"],
        "psi_us": {k: pressure(Path("/proc/pressure") / k) for k in ("memory", "io", "cpu")},
    }


def sample(group):
    cpu = fields(group / "cpu.stat")
    mem = fields(group / "memory.stat")
    return {
        "t": time.monotonic(),
        "cpu_us": cpu["usage_usec"],
        "throttled_us": cpu["throttled_usec"],
        "working_bytes": mem["anon"] + mem["shmem"],
        "swap_bytes": int((group / "memory.swap.current").read_text()),
        "total_bytes": int((group / "memory.current").read_text()),
        "pgmajfault": mem["pgmajfault"],
    }


def wait_for(test, seconds, message):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if value := test():
            return value
        time.sleep(0.08)
    raise RuntimeError(message)


def verify_display(service_group):
    display = os.environ["DISPLAY"].encode()
    if (
        os.environ.get("WAYLAND_DISPLAY")
        or os.environ.get("WAYLAND_SOCKET")
        or not Path(os.environ["XAUTHORITY"]).is_file()
    ):
        raise RuntimeError("Expected a private X11 display")
    pids = (service_group / "cgroup.procs").read_text().split()
    if str(os.getpid()) in pids:
        for pid in pids:
            process = Path("/proc") / pid
            with contextlib.suppress(FileNotFoundError):
                if (process / "comm").read_text().strip() == "Xvfb" and display in (
                    process / "cmdline"
                ).read_bytes().split(b"\0"):
                    return
    raise RuntimeError("The trial and its Xvfb display must share the bounded service cgroup")


def browser_arguments(browser, directory):
    if browser == "Ladybird":
        (directory / "config/Settings.json").write_text('{"firstRunComplete":true}')
        return ["--profile-path", str(directory)]
    if browser == "Firefox":
        (directory / "user.js").write_text(
            'user_pref("browser.shell.checkDefaultBrowser", false);\n'
            'user_pref("browser.aboutwelcome.enabled", false);\n'
            'user_pref("browser.sessionstore.resume_from_crash", false);\n'
        )
        return ["--no-remote", "--profile", str(directory)]
    return [
        "--user-data-dir=" + str(directory),
        "--ozone-platform=x11",
        "--no-first-run",
        "--no-default-browser-check",
        "--disable-background-networking",
        "--disable-component-update",
        "--disable-extensions",
        "--window-size=1440,900",
    ]


def focus_last_tab(browser, count):
    def window_id():
        result = subprocess.run(
            ["xdotool", "search", "--onlyvisible", "--name", "Tab resource lab"], capture_output=True, text=True
        )
        return next(iter(result.stdout.splitlines()), None)

    window = wait_for(window_id, 3, "Missing browser window on Xvfb")
    subprocess.run(["xdotool", "windowsize", window, "1440", "900"], check=True)
    subprocess.run(["xdotool", "windowfocus", window], check=True)
    subprocess.run(["xdotool", "key", "--clearmodifiers", "alt+9" if browser == "Firefox" else "ctrl+9"], check=True)

    def last_visible():
        title = subprocess.check_output(["xdotool", "getwindowname", window], text=True)
        return re.search(rf"Tab resource lab {count}\b", title)

    wait_for(last_visible, 3, "Final tab must be visible")


def measure(trial, group, process, browser, trace):
    def ready():
        if process.poll() is not None:
            raise RuntimeError(f"Browser exited with {process.returncode}")
        return trial.pages_ready()

    wait_for(ready, 18, "Some requested pages did not load")
    verify_limits(group)
    focus_last_tab(browser, len(trial.tabs))
    time.sleep(1)

    def sample_pressure():
        return {key: pressure(group / (key + ".pressure")) for key in ("cpu", "memory", "io")}

    trace["memory_events"] = [fields(group / "memory.events")]
    trace["pressure"] = [sample_pressure()]
    idle = trace["idle_samples"] = [sample(group)]
    while time.monotonic() - idle[0]["t"] < 8:
        time.sleep(0.125)
        idle.append(sample(group))

    active = trace["active_samples"] = []
    completed = trace["page_receipts"] = []
    for _ in range(2 if trial.dashboards else 1):
        samples = [sample(group)]
        active.append(samples)
        trial.start_phase()

        def done():
            active[-1].append(sample(group))
            return trial.receipts()

        completed.append(wait_for(done, 10, "Work did not finish"))

    if not ready():
        raise RuntimeError("Some pages disappeared during measurement")
    with trial.condition:
        for tab in trial.targets:
            if len(trial.events[(tab, "done")]) != len(active):
                raise RuntimeError(f"Unexpected number of completion receipts from tab {tab}")
    events = fields(group / "memory.events")
    trace["memory_events"].append(events)
    trace["pressure"].append(sample_pressure())
    trace["host"].append(host_sample())
    if events["oom_kill"] or events["oom"]:
        raise RuntimeError("OOM during trial")
    working = [s["working_bytes"] for samples in active for s in samples]
    trace["metrics"] = {
        "active_working_mib": statistics.median(working) / 2**20,
        "active_working_peak_mib": max(working) / 2**20,
        "cpu_active_s": sum(t[-1]["cpu_us"] - t[0]["cpu_us"] for t in active) / 1e6,
        "active_wall_s": sum(t[-1]["t"] - t[0]["t"] for t in active),
        "cpu_idle_pct": (idle[-1]["cpu_us"] - idle[0]["cpu_us"]) / (idle[-1]["t"] - idle[0]["t"]) / 1e4,
        "idle_working_mib": statistics.median(s["working_bytes"] for s in idle) / 2**20,
        "idle_swap_mib": statistics.median(s["swap_bytes"] for s in idle) / 2**20,
        "active_swap_mib": statistics.median(s["swap_bytes"] for t in active for s in t) / 2**20,
    }
    trace["work"] = {
        "scenario": "mixed" if trial.dashboards else "reading",
        "tabs": len(trial.tabs),
        "dashboards": len(trial.dashboards),
        "updates": len(trial.dashboards) * 6 * len(active),
        "scroll_frames": 0 if trial.dashboards else 120,
        "scroll_px": 0 if trial.dashboards else 1920,
    }


def stop_browser(unit, group, process, log):
    # Browser shutdown is outside the measurement. Kill only this ephemeral scope.
    subprocess.run(["systemctl", "--user", "kill", "--signal=KILL", unit], stdout=log, stderr=log, timeout=3)
    process.wait(timeout=3)

    def empty():
        try:
            return fields(group / "cgroup.events")["populated"] == 0
        except FileNotFoundError:
            return True

    wait_for(empty, 2, f"Browser processes remain in {group}")


def browser_run(args):
    if not inherited_reservation():
        raise RuntimeError("The service did not receive the host reservation")
    token = args.slice.removesuffix(".slice")
    parent = cgroup(args.slice)
    service = cgroup(token + ".service")
    if service.parent != parent:
        raise RuntimeError("Service is outside the trial slice")
    for group in (parent, service):
        verify_limits(group)
    verify_display(service)
    browser, executable, _ = browser_config(args.config, args.variant)
    trial = Trial(token, args.scenario, args.tabs)
    unit = token + "browser.scope"
    group = parent / unit
    pre = host_sample()
    time.sleep(1)
    start = host_sample()
    trace = {
        "browser": browser,
        "executable": str(executable),
        "display": os.environ["DISPLAY"],
        "host": [pre, start],
        "preexisting_paging": any(start[k] > pre[k] for k in ("swap_in_pages", "swap_out_pages")),
        "preexisting_memory_stall": (start["psi_us"]["memory"]["full"] - pre["psi_us"]["memory"]["full"]) / 1e6
        > 0.01 * (start["t"] - pre["t"]),
    }
    try:
        with (
            serve_pages(trial) as urls,
            tempfile.TemporaryDirectory(prefix="lb-perf-profile-") as profile,
            (args.output / "browser.log").open("w") as log,
        ):
            env = os.environ.copy()
            directory = Path(profile)
            for name in ("CONFIG", "CACHE", "DATA", "STATE"):
                folder = directory / name.lower()
                folder.mkdir(mode=0o700)
                env[f"XDG_{name}_HOME"] = str(folder)
            command = [
                "systemd-run",
                "--user",
                "--scope",
                "--unit=" + unit,
                "--slice=" + args.slice,
                *LIMITS,
                "--property=RuntimeMaxSec=53s",
                "--property=TimeoutStopSec=1s",
                "--property=BindsTo=" + token + ".service",
                "--property=After=" + token + ".service",
                str(executable),
                *browser_arguments(browser, directory),
                *urls,
            ]
            process = subprocess.Popen(command, env=env, stdin=subprocess.DEVNULL, stdout=log, stderr=log)
            try:
                measure(trial, group, process, browser, trace)
            finally:
                failed = sys.exc_info()[0] is not None
                try:
                    stop_browser(unit, group, process, log)
                except (OSError, RuntimeError, subprocess.SubprocessError) as error:
                    if not failed:
                        raise
                    print(f"Browser cleanup also failed: {error}", file=sys.stderr)
    except BaseException:
        (args.output / "partial-trace.json").write_text(json.dumps(trace, allow_nan=False))
        raise
    print(json.dumps(trace, allow_nan=False))


def supervise(args, lock):
    target = "lbmeasure" + secrets.token_hex(8)
    unit = target + ".slice"

    def interrupted(_signum, _frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, interrupted)
    cleanup_failed = False
    try:
        systemctl("set-property", "--runtime", unit, "CPUQuota=200%", "MemoryMax=2G")
        result = subprocess.run(
            [
                "systemd-run",
                "--user",
                "--collect",
                "--wait",
                "--quiet",
                "--pipe",
                "--unit=" + target,
                "--slice=" + unit,
                *LIMITS,
                "--property=RuntimeMaxSec=58s",
                "--property=TimeoutStopSec=1s",
                "env",
                "-u",
                "DISPLAY",
                "-u",
                "WAYLAND_DISPLAY",
                "-u",
                "WAYLAND_SOCKET",
                "-u",
                "XAUTHORITY",
                "QT_QPA_PLATFORM=xcb",
                "QT_SCALE_FACTOR=1",
                "QT_AUTO_SCREEN_SCALE_FACTOR=0",
                "xvfb-run",
                "-a",
                "-s",
                "-screen 0 1440x900x24 -nolisten tcp",
                sys.executable,
                str(Path(__file__).resolve()),
                "--config",
                str(args.config),
                "--scenario",
                args.scenario,
                "--tabs",
                str(args.tabs),
                "--variant",
                args.variant,
                "--output",
                str(args.output),
                "--slice",
                unit,
            ],
            stdin=lock,
        )
    finally:
        for command in (["stop", unit], ["--runtime", "revert", unit]):
            try:
                systemctl(*command, stdout=sys.stderr, stderr=sys.stderr, timeout=4)
            except (OSError, subprocess.SubprocessError) as error:
                print(f"Slice cleanup failed: {error}", file=sys.stderr)
                cleanup_failed = True
    return result.returncode or int(cleanup_failed)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--scenario", choices=["mixed", "reading"], required=True)
    parser.add_argument("--tabs", choices=[10, 25, 50], type=int, default=50)
    parser.add_argument("--variant", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--slice", help=argparse.SUPPRESS)
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.config = args.config.resolve()
    args.output.mkdir(exist_ok=True, parents=True)
    if args.slice:
        browser_run(args)
        return 0

    if inherited_reservation():
        return supervise(args, sys.stdin)
    try:
        with host_reservation() as lock:
            return supervise(args, lock)
    except BlockingIOError:
        print("Host reservation busy", file=sys.stderr)
        return os.EX_TEMPFAIL


if __name__ == "__main__":
    sys.exit(main())
