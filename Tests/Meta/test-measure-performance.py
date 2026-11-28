#!/usr/bin/env python3

# Copyright (c) 2026-present, the Ladybird developers.
# SPDX-License-Identifier: BSD-2-Clause

import fcntl
import json
import subprocess
import sys
import tempfile
import unittest

from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[2] / "Meta" / "measure-performance.py"
TRIAL = """
import json
import sys
from pathlib import Path

variant, block, position, output, mode, literal = sys.argv[1:]
output = Path(output)
assert literal == "literal-{variant}"
(output / "args.json").write_text(json.dumps([variant, block, position]))
print("trial diagnostics", file=sys.stderr)
work = {"steps": 7, "path": ["open", "wait", "close"]}
metrics = {"latency": int(block) + (100 if variant == "B" else 0)}
if mode == "retry" and int(block) == 1 and "attempt-1" in str(output) and variant == "A":
    sys.exit(9)
busy = Path(__file__).with_name("busy")
if mode == "mixed" and position == "1":
    sys.exit(9)
if mode in ("mixed", "defer") and busy.exists() and position == busy.read_text():
    sys.exit(75)
if mode == "work" and int(block) == 2:
    work["path"].pop()
if mode == "metrics" and int(block) == 2:
    metrics["extra"] = 1
if mode == "nan":
    metrics["latency"] = float("nan")
print(json.dumps({"metrics": metrics, "work": work, "internal_samples": [0.1, 0.1, 0.2]}))
"""


class TestMeasurePerformance(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.program = self.root / "trial.py"
        self.program.write_text(TRIAL)
        self.output = self.root / "campaign"

    def invoke(self, *options, mode="ok", expected=0):
        command = [
            sys.executable,
            str(SCRIPT),
            "--output",
            str(self.output),
            "--baseline",
            "A",
            "--candidate",
            "B",
            "--input",
            str(self.program),
            *options,
            "--",
            sys.executable,
            str(self.program),
            "{variant}",
            "{block}",
            "{order}",
            "{output}",
            mode,
            "literal-{variant}",
        ]
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, expected, result.stderr + result.stdout)
        return result

    def summary(self):
        return json.loads((self.output / "summary.json").read_bytes())

    def test_preplanned_balance_and_resume(self):
        self.invoke("--batch-blocks", "1")
        self.assertEqual(self.summary()["status"], "partial")
        self.assertNotIn("metrics", self.summary())
        plan_path = self.output / "plan.json"
        plan = plan_path.read_bytes()
        schedule = json.loads(plan)["schedule"]
        self.assertEqual(len(schedule), 6)
        self.assertEqual(schedule.count(["A", "B"]), 3)
        self.assertEqual(schedule.count(["B", "A"]), 3)
        self.assertEqual(len(list(self.output.glob("*/[12]/result.json"))), 2)
        receipt = self.output / "block-1-attempt-1/1/result.json"
        original_time = receipt.stat().st_mtime_ns
        self.invoke("--batch-blocks", "6")
        self.invoke()
        summary = self.summary()
        self.assertEqual(summary["status"], "complete")
        self.assertEqual(len(summary["accepted_blocks"]), 6)
        self.assertEqual(len(list(self.output.glob("*/[12]/result.json"))), 12)
        self.assertEqual(plan_path.read_bytes(), plan)
        self.assertEqual(receipt.stat().st_mtime_ns, original_time)
        for block, order in enumerate(schedule, 1):
            for position, variant in enumerate(order, 1):
                args = self.output / f"block-{block}-attempt-1/{position}/artifacts/args.json"
                self.assertEqual(json.loads(args.read_bytes()), [variant, str(block), str(position)])
        latency = summary["metrics"]["latency"]
        self.assertEqual(latency["baseline"], {"median": 3.5, "mad": 1.5, "min": 1, "max": 6})
        self.assertEqual(latency["candidate"]["median"], 103.5)
        self.assertEqual(latency["candidate_minus_baseline"], [100] * 6)
        self.assertEqual(latency["paired_absolute_difference"]["mad"], 0)

    def test_retry_replaces_both_halves(self):
        self.invoke("--batch-blocks", "1", mode="retry")
        summary = self.summary()
        self.assertEqual(summary["accepted_blocks"], [])
        self.assertEqual(len(summary["excluded_attempts"]), 1)
        self.assertEqual(len(list(self.output.glob("*/[12]/stderr"))), 2)
        self.invoke("--batch-blocks", "6", mode="retry")
        summary = self.summary()
        self.assertEqual(summary["status"], "complete")
        self.assertEqual(summary["accepted_blocks"][0], {"block": 1, "attempt": 2})
        self.assertEqual(len(list(self.output.glob("*/[12]/result.json"))), 14)

    def test_interruption_consumes_an_attempt(self):
        self.invoke("--batch-blocks", "1")
        interrupted = self.output / "block-2-attempt-1"
        interrupted.mkdir()
        self.invoke("--batch-blocks", "1")
        summary = self.summary()
        self.assertEqual(summary["accepted_blocks"][-1], {"block": 2, "attempt": 2})
        self.assertEqual(len(summary["excluded_attempts"][0]["errors"]), 2)
        self.assertEqual(list(interrupted.iterdir()), [])

    def test_busy_host_defers_whole_pair_until_resume(self):
        busy = self.root / "busy"
        for attempt, position in enumerate((1, 2, 2), 1):
            busy.write_text(str(position))
            self.invoke("--batch-blocks", "6", mode="defer", expected=75)
            summary = self.summary()
            self.assertEqual(summary["status"], "deferred")
            self.assertEqual(summary["excluded_attempts"], [])
            self.assertEqual(summary["accepted_blocks"], [])
            self.assertEqual(summary["deferred_attempts"][-1], {"block": 1, "attempt": attempt, "order": position})
            directory = self.output / f"block-1-attempt-{attempt}"
            self.assertEqual(len(list(directory.glob("*/result.json"))), position)
        first_half = self.output / "block-1-attempt-2/1/result.json"
        original = first_half.read_bytes()
        self.assertEqual(json.loads(original)["returncode"], 0)
        busy.unlink()
        self.invoke("--batch-blocks", "6", mode="defer")
        summary = self.summary()
        self.assertEqual(summary["status"], "complete")
        self.assertEqual(len(summary["deferred_attempts"]), 3)
        self.assertEqual(summary["accepted_blocks"][0], {"block": 1, "attempt": 4})
        self.assertEqual(first_half.read_bytes(), original)
        self.assertEqual(len(list((self.output / "block-1-attempt-4").glob("*/result.json"))), 2)

    def test_deferral_preserves_real_failure_of_other_half(self):
        (self.root / "busy").write_text("2")
        self.invoke(mode="mixed", expected=75)
        self.invoke(mode="mixed", expected=1)
        summary = self.summary()
        self.assertEqual(summary["status"], "failed")
        self.assertEqual(len(summary["excluded_attempts"]), 2)
        self.assertEqual(len(summary["deferred_attempts"]), 2)

    def test_rejects_receipts_from_other_trials(self):
        self.invoke("--batch-blocks", "1")
        directory = self.output / "block-1-attempt-1"
        first = directory / "1/result.json"
        original = json.loads(first.read_bytes())
        for key, value in (("block", 2), ("order", 2), ("variant", "another label")):
            with self.subTest(key=key):
                first.write_text(json.dumps({**original, key: value}))
                self.assertIn("identity does not match plan", self.invoke(expected=1).stderr)
        first.write_bytes((directory / "2/result.json").read_bytes())
        self.assertIn("identity does not match plan", self.invoke(expected=1).stderr)
        self.assertFalse((self.output / "block-2-attempt-1").exists())

    def test_changed_work_or_metrics_exhaust_pair(self):
        for mode in ("work", "metrics", "nan"):
            with self.subTest(mode=mode):
                self.output = self.root / mode
                self.invoke("--batch-blocks", "6", mode=mode, expected=1)
                summary = self.summary()
                self.assertEqual(summary["status"], "failed")
                self.assertEqual(len(summary["excluded_attempts"]), 2)
                self.assertNotIn("metrics", summary)
                self.invoke("--batch-blocks", "1", mode=mode, expected=1)
                self.assertEqual(self.summary(), summary)

    def test_resume_rejects_changed_configuration_and_input(self):
        self.invoke("--batch-blocks", "1")
        summary = self.summary()
        self.invoke("--seed", "9", expected=1)
        self.program.write_text(TRIAL + "\n")
        self.invoke(expected=1)
        self.assertEqual(self.summary(), summary)
        self.assertEqual(len(list(self.output.glob("*/[12]/result.json"))), 2)

    def test_minimum_sample_count_and_exclusive_output(self):
        self.invoke("--runs", "4", expected=2)
        self.assertFalse(self.output.exists())
        self.output.mkdir()
        with (self.output / ".lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            self.assertIn("another controller", self.invoke(expected=1).stderr)
        self.assertFalse((self.output / "plan.json").exists())


if __name__ == "__main__":
    unittest.main()
