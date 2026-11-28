#!/usr/bin/env python3

# Copyright (c) 2026-present, the Ladybird developers.
# SPDX-License-Identifier: BSD-2-Clause

"""Compare two labeled variants in preplanned pairs of independent process runs (POSIX).

After --, supply the command that runs ONE entire trial. It must reserve the shared
host lock, verify/enforce CPU, memory and wall-time limits for its process group,
set up the display if needed, and clean up. Size --batch-blocks for the batch time
budget. The command must exit successfully and print one JSON object (up to 1 MiB)
with a nonempty "metrics" object of finite numbers and a nonempty "work" object
identifying the complete workload and its completed work counters. Write large
traces under {output}. Internal samples contribute to one trial, never extra pairs.

Only whole arguments {variant}, {block}, {order}, {output} are replaced. Block
numbers and positions start at 1. For an A/A noise control use two labels mapped
to the same executable. Use --input for the wrapper, workloads, both binaries
and other files that identify the experiment. Resume with the same arguments.
A failed or interrupted pair gets one fresh attempt in its original order.
If the host is busy, the command must exit with EX_TEMPFAIL (75). The controller
then exits with 75. Resume later to run the entire pair in the same order. Evidence
from any completed half is retained. Only actual failures consume a retry.
A completed summary reports the median, median absolute deviation (MAD) and
extrema in each metric's units. Absolute paired differences in an A/A control
describe the observed run-to-run noise for that workload.
"""

import argparse
import fcntl
import hashlib
import json
import math
import os
import random
import statistics
import subprocess
import sys
import time

from itertools import count
from pathlib import Path


def write_json(path, value):
    temporary = path.with_suffix(".tmp")
    with temporary.open("w") as stream:
        json.dump(value, stream, indent=2, allow_nan=False)
        stream.write("\n")
    temporary.replace(path)


def file_hash(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def make_plan(args):
    randomizer = random.Random(args.seed)
    schedule = []
    while len(schedule) < args.runs:
        order = [args.baseline, args.candidate]
        randomizer.shuffle(order)
        schedule.extend([order, order[::-1]])
    return {
        "baseline": args.baseline,
        "candidate": args.candidate,
        "seed": args.seed,
        "schedule": schedule[: args.runs],
        "command": args.command,
        "cwd": str(Path.cwd()),
        "controller_sha256": file_hash(Path(__file__)),
        "input_sha256": {str(path.resolve()): file_hash(path) for path in args.input},
    }


def run_trial(command, directory, variant, block, order):
    directory.mkdir()
    artifacts = directory / "artifacts"
    artifacts.mkdir()
    values = {"{variant}": variant, "{block}": str(block), "{order}": str(order), "{output}": str(artifacts)}
    argv = [values.get(arg, arg) for arg in command]
    receipt = {"variant": variant, "block": block, "order": order, "start_unix_seconds": time.time()}
    start = time.monotonic()
    stdout_path = directory / "stdout"
    try:
        with stdout_path.open("wb") as stdout, (directory / "stderr").open("wb") as stderr:
            receipt["returncode"] = subprocess.run(argv, stdout=stdout, stderr=stderr).returncode
        if receipt["returncode"]:
            raise ValueError(f"command exited with {receipt['returncode']}")
        if stdout_path.stat().st_size > 1024 * 1024:
            raise ValueError("stdout exceeds 1 MiB")
        payload = json.loads(stdout_path.read_bytes())
        if not isinstance(payload, dict):
            raise ValueError("expected a JSON object")
        work, metrics = payload.get("work"), payload.get("metrics")
        if not isinstance(work, dict) or not work:
            raise ValueError("expected a nonempty work object")
        json.dumps(work, allow_nan=False)
        if not isinstance(metrics, dict) or not metrics:
            raise ValueError("expected a nonempty metrics object")
        if not all(type(x) in (int, float) and math.isfinite(x) for x in metrics.values()):
            raise ValueError("expected finite numeric metrics")
        receipt.update(work=work, metrics=metrics)
    except (OSError, ValueError, OverflowError, RecursionError) as error:
        receipt["error"] = str(error)
    receipt["elapsed_seconds"] = time.monotonic() - start
    write_json(directory / "result.json", receipt)
    return receipt.get("returncode")


def distribution(samples):
    median = statistics.median(samples)
    return {
        "median": median,
        "mad": statistics.median(abs(value - median) for value in samples),
        "min": min(samples),
        "max": max(samples),
    }


def summarize(pairs, baseline, candidate):
    metrics = {}
    for name in pairs[0][baseline]:
        before = [pair[baseline][name] for pair in pairs]
        after = [pair[candidate][name] for pair in pairs]
        differences = [b - a for a, b in zip(before, after)]
        metrics[name] = {
            "baseline": distribution(before),
            "candidate": distribution(after),
            "candidate_minus_baseline": differences,
            "paired_difference": distribution(differences),
            "paired_absolute_difference": distribution([abs(delta) for delta in differences]),
        }
    return metrics


def run_campaign(args):
    root = args.output
    plan = make_plan(args)
    manifest = root / "plan.json"
    if manifest.exists():
        if json.loads(manifest.read_bytes()) != plan:
            raise ValueError("configuration or input hashes changed; use a new output directory")
    else:
        if any(entry.name not in (".lock", "plan.tmp") for entry in root.iterdir()):
            raise ValueError("output directory already contains files without a plan")
        write_json(manifest, plan)

    summary = {
        "status": "partial",
        "planned_pairs": args.runs,
        "accepted_blocks": [],
        "excluded_attempts": [],
        "deferred_attempts": [],
    }
    pairs = []
    reference = None
    remaining = args.batch_blocks
    for block, order in enumerate(plan["schedule"], 1):
        failure_count = 0
        for attempt in count(1):
            directory = root / f"block-{block}-attempt-{attempt}"
            new_attempt = not directory.exists()
            if new_attempt:
                if remaining == 0:
                    return summary
                remaining -= 1
                directory.mkdir()
                for position, variant in enumerate(order, 1):
                    if run_trial(args.command, directory / str(position), variant, block, position) == os.EX_TEMPFAIL:
                        break

            pair = {}
            errors = []
            identity = reference
            deferred_at = None
            for position, variant in enumerate(order, 1):
                receipt = directory / str(position) / "result.json"
                if not receipt.exists():
                    errors.append(f"{variant}: interrupted trial")
                    continue
                result = json.loads(receipt.read_bytes())
                if (
                    not isinstance(result, dict)
                    or type(result.get("block")) is not int
                    or type(result.get("order")) is not int
                    or (result["block"], result["order"], result.get("variant")) != (block, position, variant)
                ):
                    raise ValueError(f"{receipt}: trial identity does not match plan")
                if result.get("returncode") == os.EX_TEMPFAIL:
                    deferred_at = position
                    break
                if "error" in result:
                    errors.append(f"{variant}: {result['error']}")
                    continue
                work = (json.dumps(result["work"], sort_keys=True), sorted(result["metrics"]))
                if identity is not None and work != identity:
                    errors.append(f"{variant}: work identity or metric names changed")
                identity = work
                pair[variant] = result["metrics"]
            if errors:
                summary["excluded_attempts"].append({"block": block, "attempt": attempt, "errors": errors})
                failure_count += 1
            if deferred_at is not None:
                summary["deferred_attempts"].append({"block": block, "attempt": attempt, "order": deferred_at})
            if failure_count == 2:
                summary["status"] = "failed"
                return summary
            if deferred_at is not None and new_attempt:
                summary["status"] = "deferred"
                return summary
            if errors or deferred_at is not None:
                continue
            assert identity is not None
            reference = identity
            summary["work"] = json.loads(identity[0])
            pairs.append(pair)
            summary["accepted_blocks"].append({"block": block, "attempt": attempt})
            break

    summary["status"] = "complete"
    summary["metrics"] = summarize(pairs, args.baseline, args.candidate)
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", required=True, type=lambda path: Path(path).resolve())
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--candidate", required=True)
    parser.add_argument("--runs", type=int, default=6, help="planned pairs (default 6, minimum 5)")
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument(
        "--input", type=Path, action="append", default=[], help="fingerprint an input file (repeatable)"
    )
    parser.add_argument(
        "--batch-blocks", type=int, default=2, help="maximum new pair attempts this invocation (default 2)"
    )
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.command[:1] != ["--"] or len(args.command) < 2:
        parser.error("supply a trial command after --")
    args.command = args.command[1:]
    if args.runs < 5 or args.batch_blocks < 1 or args.baseline == args.candidate:
        parser.error("supply at least five pairs, a positive batch size and two distinct labels")
    try:
        args.output.mkdir(parents=True, exist_ok=True)
        with (args.output / ".lock").open("a") as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                raise ValueError("another controller holds this output directory") from None
            summary = run_campaign(args)
            write_json(args.output / "summary.json", summary)
    except (OSError, ValueError) as error:
        parser.exit(1, f"{parser.prog}: {error}\n")
    print(f"{summary['status']}: {len(summary['accepted_blocks'])}/{args.runs} pairs; {args.output / 'summary.json'}")
    if summary["status"] == "deferred":
        return os.EX_TEMPFAIL
    return int(summary["status"] == "failed")


if __name__ == "__main__":
    sys.exit(main())
