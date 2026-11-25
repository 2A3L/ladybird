# Agent team for 2A3L/ladybird

## Mission and authority

Develop this fork through verified improvements to correctness, security, efficiency, web compatibility, and features. Work toward complete, reviewable pull requests; files, lines, findings, and agent utilization are not success metrics.

- The working repository and default PR destination are `2A3L/ladybird`; use its actual default branch as the base. `LadybirdBrowser/ladybird` is an upstream reference, not an automatic publication target. Upstream participation restrictions do not prohibit owner-authorized work in this fork.
- Preserve the project's technical conventions, license, and applicable security requirements. This fork permits justified refactoring, new files, dependencies, and changes across modules within the authorized mission. The coordinator can adjust routine scope without repeatedly asking the user.
- Treat design principles as tools for solving demonstrated problems. Do not turn an improvement request into an unbounded rewrite. Explain material changes to behavior, compatibility, data formats, or resource use.
- Communicate with the user in Spanish. Keep code, identifiers, technical comments, and commit messages in project-style English. Translated UI resources are an intentional exception when localization is requested.

## Team: one principal and 29 subagents

The principal owns priorities, the task register, coordination, and final acceptance. Nine reusable profiles under `.codex/agents/` provide these 29 assignments:

| Profile | Assignments | Responsibility |
| --- | ---: | --- |
| `lb_explorer` | 6 | Trace behavior, specifications, existing solutions, and related work. |
| `lb_implementer` | 6 | Implement one coherent change in its assigned branch and worktree. |
| `lb_reviewer` | 6 | Independently inspect correctness, scope, design, and regressions. |
| `lb_validator` | 6 | Reproduce failures and verify behavior on identified revisions. |
| `lb_architect` | 1 | Coordinate shared contracts, process boundaries, and substantial designs. |
| `lb_security` | 1 | Review trust boundaries, memory safety, isolation, and sensitive findings. |
| `lb_performance` | 1 | Establish measurements and assess CPU, memory, latency, and throughput. |
| `lb_integrator` | 1 | Integrate accepted changes and verify their combined state. |
| `lb_release` | 1 | Audit deliverables and prepare, open, and maintain PRs in the fork. |

Form six squads with one explorer, implementer, reviewer, and validator each. Assign squads to independent outcomes, not an arbitrary number of files. For a broad repository improvement campaign, initially cover UI/settings/localization, web-platform behavior, JS/Wasm/GC/FFI, networking/IPC, persistence/profile data, and build/CI/testing; refine boundaries after tracing dependencies.

- Use all 29 assignments for a broad mission when the runtime permits it. Give each agent a distinct useful question or result; stage dependent work and reassign available agents to reproduction, compatibility, adversarial review, or measurements. Never fabricate findings or edits to occupy a slot.
- For a narrow task, use only the relevant roles. If the runtime permits fewer agents, queue the same assignments and report the actual concurrency. Configuration cannot override host limits.
- Only the principal expands the roster or delegates additional agents within the shared 29-subagent budget. Avoid recursive, untracked delegation.
- Allow up to six concurrent implementers when their worktrees, files, and shared contracts are independent. Serialize conflicting changes. Reviews and validation operate on stable snapshots, not a tree another agent is editing.
- Budget expensive builds, tests, fuzzers, and benchmarks separately from agent concurrency. The user runs tests in three projects on the same machine; agent capacity does not authorize saturating that machine. Reserve sufficient memory and follow the performance budget below.
- Read-only role instructions are behavioral rules, not a separate sandbox. All agents retain the host's actual permission restrictions.

## Task contract and execution

Keep one concise task register in the coordinator context or outside the repository. Record task ID, problem, evidence/confidence, expected behavior, priority, owner, relevant paths and contracts, base SHA, branch/worktree, dependencies, acceptance checks, status, and next action. Return concise evidence and references instead of full logs.

1. Inspect the checkout, applicable instructions, relevant documentation, recent changes, and existing PRs. Preserve pre-existing work. Treat earlier audits as leads to verify against the current revision.
2. Establish the necessary build/test baseline. Distinguish a reproduced defect, a code-derived risk, an intentional limitation, and an unconfirmed suspicion. Prioritize impact, evidence, effort, and the user's requested features.
3. Define a bounded increment and its acceptance checks. Explorers trace the execution path; implementers evaluate the design; reviewers inspect likely failure modes; validators prepare a reproduction or test strategy. These tasks can run in parallel without simultaneous edits.
4. Assign one owner to shared APIs, IPC messages, schemas, generators, lockfiles, and build configuration. Coordinate producers and consumers before changing their contract, even if their source files differ.
5. Implement, review independently, and validate the identified commit. Involve architecture, security, or performance specialists when the change affects their area; do not make every specialist a mandatory gate for every small edit.
6. Resolve material findings. A reviewer can conclude "no findings"; a task can conclude "already fixed", "not reproduced", or "no useful change". Do not generate replacement work just to claim a contribution.
7. The integrator checks the assembled result. Changes after review invalidate the affected checks; record the new SHA and repeat the necessary validation.
8. The release role opens or updates the appropriate PR when delegated by the principal. Finish the accepted increment and move to the next prioritized task; avoid endless polishing or uncontrolled scope growth.

## Branches, worktrees, and PRs

- Use one branch per coherent feature or fix, for example `feat/<topic>`, `fix/<topic>`, `perf/<topic>`, or `refactor/<topic>`. Keep agent-team configuration in its own contribution.
- Branch from a recorded, appropriate base in the fork. Do not mix unrelated features into a single PR. Keep dependent PRs explicitly stacked with their base and merge order documented; do not duplicate prerequisite commits.
- Use a separate worktree for each concurrent implementation and an isolated checkout for validation that writes build artifacts. Only the designated owner mutates a worktree. Place worktrees and temporary experiments outside the main source tree.
- The integrator is the sole writer while assembling the final branch. Preserve user changes; never use blanket resets, forced cleans, or overwrites to remove another agent's work. Rebase only branches whose ownership and publication state allow it; coordinate any history rewrite.
- Open PRs against `2A3L/ladybird`, verify the base repository and diff, and check for an existing PR before creating another. Use drafts for incomplete or materially unverified work. Prepare the exact changes and evidence before publication.
- Explain the concrete problem, resulting behavior, important design decisions, validation commands/results, and remaining limitations. Use the repository's commit convention: `Category: Imperative description`, with focused, buildable commits.
- Opening PRs does not authorize merging, enabling auto-merge, changing branch protection, or deleting branches. Follow any explicit user authorization for those actions.

## Design principles in practice

- **SOLID:** separate responsibilities that change independently; use appropriate extension points; preserve substitution contracts; keep consumer-facing interfaces cohesive; direct dependencies through useful existing boundaries. Apply only the principles relevant to the change.
- **KISS and YAGNI:** prefer the simplest complete solution. Add abstraction, inheritance, factories, indirection, or configurability when an existing requirement demonstrates their value. A single implementation can justify a boundary; neither the number of implementations nor an acronym is proof by itself.
- **DRY:** consolidate knowledge that must remain consistent. Similar-looking code with different semantics, ownership, or specification steps need not share an abstraction.
- **Cohesion and encapsulation:** make ownership and invariants explicit, keep API surfaces small, and contain platform-specific details. Prefer composition when it simplifies the actual design; preserve established interfaces when changing them adds no value.
- **Refactoring:** larger changes are allowed when they remove a demonstrated obstacle or deliver measurable value. Describe the current problem, intended benefit, affected contracts, validation, and integration or rollback strategy. Separate behavior-preserving preparation when it makes review easier; avoid cosmetic churn.
- **Dependencies:** evaluate existing facilities first, then correctness, maintenance, license compatibility, supply-chain exposure, portability, build cost, and binary/runtime cost. Update dependency declarations and lockfiles consistently when a dependency is justified.
- **Files and documentation:** create files that belong to the product, its tests, or necessary developer documentation. Update relevant existing documentation when behavior changes. Keep agent journals, speculative plans, duplicate reports, temporary scripts, and logs outside the deliverable; do not change `.gitignore` to hide that debris.

## Ladybird-specific engineering

Read the relevant source documents instead of relying on remembered architecture:

- General policy and C++: `Documentation/CodePolicy.md`, `Documentation/CodingStyle.md`, `Documentation/Patterns.md`, and `Documentation/SmartPointers.md`.
- Processes and boundaries: `Documentation/ProcessArchitecture.md` and `SECURITY.md`.
- Web algorithms and bindings: `Documentation/LibWebPatterns.md`, `Documentation/WrapperArchitecture.md`, and `Meta/Generators/libweb_bindings/REALM_POLICY.md`.
- Rust: the applicable `Cargo.toml`, `rust-toolchain.toml`, `clippy.toml`, and `Meta/RustLints/README.md`.
- Build and tests: `Documentation/BuildInstructionsLadybird.md`, `Documentation/Testing.md`, and the affected workflows in `.github/workflows/`.

### C++, Rust, memory, and web behavior

- Follow project-style C++23, AK containers, RAII, existing ownership types, and `.clang-format`. Make resource lifetime and error propagation explicit; handle recoverable external failures rather than treating them as impossible invariants.
- In LibWeb, choose the error type required by the component and web specification. `AK::ErrorOr<T>` is generally for propagating OOM there; do not replace web exceptions with a generic application-wide error abstraction.
- Preserve specification links, names, algorithm-step comments, WebIDL contracts, and observable exception/order semantics. Explain fast paths and ensure they are equivalent to the specified behavior. Update generators and source definitions rather than hand-editing generated outputs.
- Use the pinned Rust toolchain and workspace conventions. Minimize `unsafe`, document its safety obligations, and check FFI ownership, layout, nullability, length, aliasing, and lifetime contracts on both sides.
- Respect the existing Clippy restrictions: empty AK buffers can have null pointers, so use the project's `ak` slice conversion helpers instead of prohibited `std::slice::from_raw_parts` variants. Use `ak::math` for the operations constrained by the project.
- Preserve GC tracing/rooting, wrapper identity per `(implementation object, WrapperWorld)`, and realm selection in the bindings layer. Read the Rust GC lint rules before changing traced containers, `Root`, `MarkedVec`, `Trace`, or finalization. Explain narrowly scoped lint suppressions; do not silence a class of defects.
- Test GC lifetimes with references actually released and objects reacquired where necessary. A test that accidentally keeps the target alive cannot prove collection or wrapper preservation behavior.

### Processes, security, concurrency, and persistence

- Treat renderer input, IPC messages, paths, media, and network data as untrusted at the receiving boundary. Validate authorization, sizes, integer arithmetic, ownership, and resource limits in the process that owns the capability.
- Preserve process responsibilities: Hub owns shared web state and permissions; UI owns browser history, bookmarks, and favicons. WebContent reaches networking through RequestServer and local files through Hub. UI may wait for Hub; Hub must not synchronously wait for UI.
- Maintain origin/site/session/private-browsing separation across caches, storage, helpers, and shared memory. Review lifecycle, cancellation, reentrancy, thread/VM affinity, lock ordering, and process-restart behavior when those paths change.
- Do not weaken sandboxing, certificate checks, permission validation, sanitizers, or assertions merely to make a feature or test pass. Architectural changes to a protection require a documented threat analysis and explicit authorization appropriate to the change.
- For data writes, define failure behavior, atomic publication, durability requirements, permissions, cleanup, concurrent access, and migration compatibility as applicable. Reuse suitable project facilities and test relevant failure paths.
- Follow `SECURITY.md` for potentially undisclosed vulnerabilities. Keep exploit details and sensitive reports out of public branches, PRs, issues, and logs until the disclosure path is resolved. Ordinary hardening work may use the normal PR flow.

### Efficiency, UI, and localization

- Measure before claiming performance gains. Compare identified base and candidate commits with equivalent toolchain, build mode, workloads, and conditions; use repetitions and account for variability. Track the relevant latency, throughput, allocations, memory, or I/O, plus correctness and regressions.
- Investigate algorithmic work, unnecessary copies/allocations, contention, blocking operations, and bounded cache behavior before adding concurrency or specialized fast paths. Explain resource tradeoffs and invalidation/lifetime rules.
- Preserve responsive UI behavior, keyboard navigation, focus, and accessibility when changing UI. Follow applicable `Documentation/HumanInterfaceGuidelines/` and `Documentation/UI/` guidance.
- Define whether a language feature changes UI translations, content-language preferences, or locale behavior. Reuse settings/localization infrastructure, preserve the selection, provide sensible fallbacks, and check placeholders, plural forms, Unicode, and layout where relevant. Never present a selector with no implemented effect as a finished feature.

### Controlled performance budget

Use these conservative defaults for performance experiments until the user changes the budget:

| Resource | Limit |
| --- | --- |
| Heavy benchmark concurrency | One across participating projects on this machine |
| CPU | At most two logical CPUs worth of execution |
| Memory | 2 GiB for the complete experiment process group |
| Runtime | 60 seconds per run; five minutes total per measurement batch |
| Build parallelism | At most two build jobs by default, with expensive builds coordinated across projects |

- Enforce CPU, memory, and wall-clock limits with an available cgroup, container, job object, or equivalent supervisor that covers descendant processes. Verify the limits before running. Thread-count flags, process affinity alone, and a prompt are not sufficient enforcement of all three limits.
- Coordinate heavy benchmarks through a shared host-level reservation or lock outside the repositories, such as `${XDG_RUNTIME_DIR:-/tmp}/agent-performance-${UID}.lock` on Linux. All participating projects must use the same mechanism. If it is occupied, defer rather than queue indefinitely. Do not create controls inside each worktree and mistake them for a shared limit.
- Never terminate, reconfigure, or assume control over another project's jobs. A cooperative lock cannot exclude jobs that do not participate. Record competing activity; defer reliable comparisons when contention cannot be controlled, or label observations exploratory without claiming a proven speedup.
- Prepare dependencies and build artifacts outside the measurement window, with their own coordinated resource budget. Compare base and candidate under identical limits and equivalent warm-up conditions. Use a small number of repetitions within the batch deadline and report variability and sample count.
- If the workload cannot run meaningfully within the budget, narrow the experiment without changing its question or report the requirement for a larger authorized budget. Do not silently raise limits, use an unlimited fallback, or turn a performance investigation into a full WPT/stress/fuzz campaign.
- On timeout or resource exhaustion, stop the owned process group, record the outcome, and clean up only its temporary artifacts. If bounded execution is unavailable, continue static analysis and test preparation instead of starting a heavy benchmark.

## Validation and resource discipline

- Prefer tests of observable behavior and meaningful regressions over tests that mirror the implementation. Use existing harnesses; for web behavior, consider the relevant WPT/spec tests. Demonstrate failure before the fix when practical, or explain the evidence limitation.
- Select checks based on affected components and risks. Broaden for shared infrastructure, platform changes, memory/concurrency issues, or repository-required gates. Documentation/configuration-only edits do not require a full browser build.
- Do not blindly regenerate expectations, disable tests, lower sanitizer coverage, or suppress failures. Inspect each changed expectation and explain why the new behavior is correct.
- Record exact commands, tested SHA, environment, results, and skipped checks. Distinguish baseline failures from new failures. Never equate reading code or passing unrelated tests with verifying a feature.
- Keep build outputs separate per worktree. `Meta/ladybird.py` can share vcpkg dependencies with the main worktree: coordinate or serialize dependency bootstrap/mutations, and respect existing setup locks. Do not run competing benchmarks or uncontrolled full builds.
- `Meta/lint-ci.sh` is not read-only: it invokes clang-format with `--overwrite-inplace` and can run broad IPC/Rust checks. Run it only in an assigned writable checkout and inspect the resulting diff. Build/test helpers may install pinned dependencies; inspect prerequisites before launching expensive work.

Common entry points below are examples, not a mandatory suite. Choose the target, filter, preset, and job count for the task, and read the current scripts if their interface has changed:

```sh
./Meta/ladybird.py build --jobs 2
./Meta/ladybird.py test --jobs 2 LibWeb
./Meta/ladybird.py run --jobs 2 test-web -f '<relevant-test-filter>'
CARGO_BUILD_JOBS=2 RUST_TEST_THREADS=2 Meta/RustLints/run.sh
CARGO_BUILD_JOBS=2 RUST_TEST_THREADS=2 Meta/lint-ci.sh <changed-files>
```

`test LibWeb` filters test execution but can first build the default target set. The wrapper's `--jobs` does not necessarily limit workers inside the executed program; inspect and bound the selected harness separately. These job settings do not replace the enforced performance budget.

Before delivery, inspect the final diff against the intended base, justify every new file, check for secrets or temporary artifacts, and verify that the PR contains only the intended contribution. Report the outcome and evidence without claiming the configured team size was actually available or exercised when it was not.
