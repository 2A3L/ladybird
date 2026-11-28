# Browser resource measurements on Linux

Compare two installed browsers, or repeat one build under two labels to measure A/A variability. Each run starts a fresh browser profile, opens 10, 25 or 50 tabs together on loopback, focuses the final tab and samples eight seconds of idle CPU and memory. A mixed run at 50 tabs has 27 dashboards, each completing six DOM and canvas updates in each of two phases. A reading run scrolls the final article by 16 CSS pixels in each of 120 animation frames, reaching 1,920 CSS pixels.

The host needs Python 3.9 or later, Linux cgroup v2 with CPU, memory, swap and PSI accounting, a systemd user manager, GNU `timeout`, `Xvfb`, `xvfb-run`, `xauth` and `xdotool`. Each trial verifies its private 1440 × 900 Xvfb display and CPU and memory limits. A systemd service and the browser's scope share a slice capped at two CPUs and 2 GiB. The service has a 58-second runtime limit. Browser samples cover its scope, including its child processes. The HTTP server and display server run in the sibling service.

Save a configuration file such as `browsers.json`. Paths may be absolute or relative to that file. Include the helpers and libraries relevant to the change in `fingerprint`. When the executable is a shell launcher, list the native browser executable it starts. A package upgrade can also change shared libraries and helper processes independently of the launcher. For example, a Firefox installation might use `/usr/bin/firefox` as the launcher and require both `/usr/lib/firefox/firefox` and `/usr/lib/firefox/libxul.so` in `fingerprint`. Check these paths against your installation. The controller always hashes the launcher, fixture, configuration, and executables. Browser presets are `Ladybird`, `Firefox`, `Chromium`, `Chrome`, `Edge` and `Vivaldi`.

```json
{
  "before": {
    "browser": "Ladybird",
    "executable": "before/bin/Ladybird",
    "fingerprint": ["before/libexec/WebContent", "before/libexec/Hub", "before/lib/liblagom-web.so"]
  },
  "after": {
    "browser": "Ladybird",
    "executable": "after/bin/Ladybird",
    "fingerprint": ["after/libexec/WebContent", "after/libexec/Hub", "after/lib/liblagom-web.so"]
  }
}
```

```sh
python3 Meta/Benchmarks/measure-browser.py --config browsers.json \
    --baseline before --candidate after --scenario mixed --tabs 50 \
    --runs 6 --output /tmp/ladybird-comparison
```

For A/A, point both labels to the same executable and files. The defaults are six pairs, the mixed workload and 50 tabs. At least five pairs are required. Omit `--output` for a new temporary results directory. To resume, supply the printed directory and the same arguments and inputs. Each batch reserves the shared `agent-performance-<uid>.lock` in `$XDG_RUNTIME_DIR` (or `/tmp`) before starting the paired controller. A busy reservation returns 75 without starting a pair. The launcher runs up to two pair attempts per batch with a 290-second deadline and releases the reservation between batches.

`summary.json` records the accepted pairs, exclusions and paired differences. Each trial's `stdout` contains its host and browser cgroup samples, memory events, page receipts and metrics. Trial diagnostics go to `stderr` and `artifacts/browser.log`. A recoverable measurement error also leaves the available samples in `artifacts/partial-trace.json`.

Working memory is cgroup anonymous plus shared memory. Active memory is the median and maximum of the observed active samples. Swap is recorded separately. Active CPU is the sum of CPU time across complete work phases, and active wall time includes polling for their completion. The fixture checks each page's initial content and rejects reported reloads, page errors, incomplete work, incorrect scrolling and browser-scope OOM. Passive articles do not perform a final activity check. The host snapshots flag paging and memory stalls before launch. A shared reservation only coordinates workloads using the same lock.
