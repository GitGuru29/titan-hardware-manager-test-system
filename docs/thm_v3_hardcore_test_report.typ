#set page(
  paper: "a4",
  margin: (x: 1.8cm, y: 2.0cm),
  header: context {
    if counter(page).get().first() > 1 {
      text(8.5pt, fill: rgb("#64748b"))[
        *ArchTitan OS Engineering Report* | THM v3 Hardcore Real-World Test Matrix (39 Scenarios)
        #h(1fr)
        September 2026
      ]
    }
  },
  footer: context {
    text(8.5pt, fill: rgb("#64748b"))[
      ArchTitan Core OS Architecture | Ground Truth & Auditable Verification
      #h(1fr)
      Page #counter(page).display() of #counter(page).final().first()
    ]
  }
)

#set text(
  font: "Liberation Sans",
  size: 9.5pt,
  fill: rgb("#1e293b"),
  spacing: 120%
)

#set heading(numbering: "1.1")
#show heading: it => [
  #v(0.35cm)
  #text(fill: rgb("#0f172a"), weight: "bold")[#it]
  #v(0.15cm)
]
#show heading.where(level: 1): it => [
  #v(0.5cm)
  #text(fill: rgb("#0369a1"), size: 13pt, weight: "bold")[#it]
  #v(0.25cm)
]
#show heading.where(level: 2): it => [
  #v(0.35cm)
  #text(fill: rgb("#0284c7"), size: 11pt, weight: "bold")[#it]
  #v(0.15cm)
]

#set table(
  stroke: (x, y) => if y == 0 { (bottom: 1.5pt + rgb("#0284c7")) } else { 0.5pt + rgb("#cbd5e1") },
  fill: (x, y) => if y == 0 { rgb("#f1f5f9") } else if calc.even(y) { rgb("#f8fafc") } else { none },
  inset: (x: 6pt, y: 5pt)
)

// ── Title Header ─────────────────────────────────────────────────────────────
#align(center)[
  #block(
    fill: rgb("#0f172a"),
    inset: 18pt,
    radius: 6pt,
    width: 100%
  )[
    #text(fill: rgb("#38bdf8"), size: 9pt, weight: "bold", tracking: 2pt)[
      ARCHTITAN LINUX HARDWARE MANAGEMENT SUBSYSTEM
    ] \
    #v(4pt)
    #text(fill: white, size: 17pt, weight: "bold")[
      THM v3 Hardcore Real-World Test Suite & Failure-Oriented Matrix
    ] \
    #v(4pt)
    #text(fill: rgb("#94a3b8"), size: 9pt)[
      Evaluation of 39 Failure-Oriented Scenarios across 13 Subsystem Groups with Auditable Decision Trails
    ]
  ]
]

#v(0.3cm)

#grid(
  columns: (1fr, 1fr, 1fr),
  gutter: 10pt,
  block(fill: rgb("#f0fdf4"), stroke: 1pt + rgb("#86efac"), inset: 8pt, radius: 4pt)[
    #text(weight: "bold", fill: rgb("#166534"))[Tested Scenarios]\
    *39 Scenarios* (Groups A to M)
  ],
  block(fill: rgb("#eff6ff"), stroke: 1pt + rgb("#93c5fd"), inset: 8pt, radius: 4pt)[
    #text(weight: "bold", fill: rgb("#1e40af"))[Invariant Adherence]\
    *28 PASS* | *10 COND_PASS* | *1 FAIL*
  ],
  block(fill: rgb("#fef2f2"), stroke: 1pt + rgb("#fca5a5"), inset: 8pt, radius: 4pt)[
    #text(weight: "bold", fill: rgb("#991b1b"))[Architectural Issues]\
    *11 Specific Issues Identified*
  ]
)

= Executive Summary

This report documents the empirical execution of the *39 hardcore failure-oriented test scenarios* against the un-modified C++20 codebase of *Titan Hardware Manager v3 (THM v3)* on ArchTitan Linux. Testing was executed using real Linux kernel primitives: child processes spinning on CPU cores, uninterruptible disk sleeps, POSIX `SIGSTOP`/`SIGCONT` signal delivery, `cgroup.procs` slice migrations, and multi-workspace Hyprland topologies.

Crucially, testing was performed *strictly with zero code modifications to THM*. The test harness exercised `libthm_core.a` as compiled to reveal both the architectural strengths of the execution-first paradigm and the practical edge-cases, failure modes, and latent vulnerabilities on real 16 GB developer workstations.

= Auditable Decision Trail Architecture

Every policy decision during test execution was logged into an auditable trace:
```
[TIMESTAMP] WS[ID] | WORKLOAD | STATE | CPU% | RSS | PRESSURE | DECISION | SLICE | SIGNAL | REASON
```
*Representative audit records from real OS execution:*
- `[22:53:35] WS2 | Gradle | BG_EXEC | CPU:85% | RSS:1200MB | P:NORMAL | DEC:KEEP_BACKGROUND | SLICE:archtitan-bg | SIG:NONE | Background build uninterrupted when user switches to WS1`
- `[22:53:35] WS3 | Idle-Helper | IDLE | CPU:0% | RSS:400MB | P:CRITICAL | DEC:FREEZE | SLICE:archtitan-fzn | SIG:SIGSTOP | Idle processes frozen to liberate RAM for active agents`
- `[22:53:37] WS3 | Resumed-Worker | RECLAIMABLE | CPU:25% | RSS:150MB | P:NORMAL | DEC:ABORT_RECLAIM | SLICE:archtitan-bg | SIG:NONE | Reclaim aborted during grace period because activity resumed`
- `[22:53:42] WS8 | TitanMirror | ACTIVE | CPU:12% | RSS:180MB | P:CRITICAL | DEC:KEEP_FULL | SLICE:archtitan-act | SIG:NONE | TitanMirror hard-block protected: immune from freeze and cgroup demotion`

= Comprehensive 39-Scenario Evaluation Matrix

#table(
  columns: (0.7fr, 1fr, 3.2fr, 1.1fr, 1.2fr),
  [*ID*], [*Group*], [*Scenario Description*], [*Status*], [*Issue Ref*],

  [TC-01], [Group A], [Two Full Dev Environments (Web WS1 <-> Android WS2)], [COND_PASS], [ISSUE-HC-01],
  [TC-02], [Group A], [Three Simultaneous Dev Stacks Under Rapid Switching], [PASS], [None],
  [TC-03], [Group A], [Ten-Workspace Monster Invariance (WS1..WS10)], [COND_PASS], [ISSUE-HC-03],

  [TC-04], [Group B], [Background Gradle + Foreground Web Build], [PASS], [None],
  [TC-05], [Group B], [Five Builds Simultaneously Active (npm, Gradle, Cargo, CMake, Py)], [COND_PASS], [ISSUE-HC-04],
  [TC-06], [Group B], [Build With Almost Zero CPU (I/O & Network Wait)], [COND_PASS], [ISSUE-HC-06],
  [TC-07], [Group B], [AI Agent Spawning Deep Toolchain Hierarchy], [PASS], [None],
  [TC-08], [Group B], [Two AI Agents Contending Under Critical RAM (90-95%)], [PASS], [None],
  [TC-09], [Group B], [AI Agent Becomes Dormant Mid-Session], [PASS], [None],

  [TC-10], [Group C], [40 Browser Tabs in Background Workspace], [COND_PASS], [ISSUE-HC-05],
  [TC-11], [Group C], [Browser as WebRTC Video Call in Background], [COND_PASS], [ISSUE-HC-06],

  [TC-12], [Group D], [Docker Development Stack (DBs, Nginx, APIs)], [COND_PASS], [ISSUE-HC-07],
  [TC-13], [Group D], [Docker + Android Build + AI Under 14GB Saturation], [PASS], [None],

  [TC-14], [Group E], [Virtual Machine in Background Workspace (QEMU/KVM 8GB)], [COND_PASS], [ISSUE-HC-08],
  [TC-15], [Group E], [VM + Browser + Android Emulator Under Pressure], [PASS], [None],

  [TC-16], [Group F], [TitanMirror Latency-Sensitive Screen Mirroring], [PASS], [None],
  [TC-17], [Group F], [TitanShare Multi-GB Transfer Under Pressure], [PASS], [None],
  [TC-18], [Group F], [Titan Shield Under Hostile Resource Pressure], [PASS], [None],
  [TC-19], [Group F], [Titan AI Coexisting With External Agents], [PASS], [None],

  [TC-20], [Group G], [Music Playback During Heavy Compilation Under Pressure], [PASS], [None],
  [TC-21], [Group G], [Bluetooth Peripheral Flood & Rapid Switching], [PASS], [None],

  [TC-22], [Group H], [16 GB RAM Saturation Enforcement Order], [PASS], [None],
  [TC-23], [Group H], [RAM Pressure While Build at Final Stage (Gradle 95%)], [PASS], [None],
  [TC-24], [Group H], [AI Launches Compiler Under Memory Pressure], [PASS], [None],

  [TC-25], [Group I], [Process Resumes Work During Reclaim Grace Period], [PASS], [None],
  [TC-26], [Group I], [New Child Appears During Reclaim Grace Period], [PASS], [None],
  [TC-27], [Group I], [Workspace Switch During Reclaim Grace Period], [PASS], [None],
  [TC-28], [Group I], [Rapid Workspace Oscillation (WS1 <-> WS2 200x)], [COND_PASS], [ISSUE-HC-09],

  [TC-29], [Group J], [Application Closes While Child Continues], [PASS], [None],
  [TC-30], [Group J], [Child Reparented to PID 1 Ownership Recovery], [PASS], [None],
  [TC-31], [Group J], [Process Changes Workload Context (npm -> cargo)], [COND_PASS], [ISSUE-HC-10],

  [TC-32], [Group K], [Workspace Number Reassignment Invariance], [PASS], [None],
  [TC-33], [Group K], [Same Workload Spanning Multiple Workspaces], [PASS], [None],
  [TC-34], [Group K], [One Workspace Containing Five Workload Types], [PASS], [None],

  [TC-35], [Group L], [THM Starts Last (Late Daemon Initialization)], [PASS], [None],
  [TC-36], [Group L], [THM Daemon Crash During Active Compilation], [PASS], [None],
  [TC-37], [Group L], [THM Crashes While Processes Are Frozen], [FAIL], [ISSUE-HC-11],

  [TC-38], [Group M], [The 'Real Developer' Machine (10 Concurrent Workspaces)], [PASS], [None],
  [TC-39], [Group M], [The 'Everything is on Fire' Nightmare Saturation Benchmark], [PASS], [None],
)

= Detailed Analysis of All Identified Issues

During the execution of the 39 failure-oriented scenarios on the unmodified THM v3 binary, *11 specific architectural gaps and failure points* were surfaced:

== Issue HC-01: Headless Databases & Emulators in Background Workspaces
- *Symptom*: When switching away from WS2 (Android), an idle Android Emulator (`qemu-system-x86_64`) or headless service (PostgreSQL, Redis) consumes under 5% CPU.
- *Root Cause*: `ExecutionDetector` defaults to `idle_cpu_pct = 5.0%`. After 3 ticks without user input or CPU burst, the emulator flips to `ActivityState::IDLE`. Under `PressureLevel::HIGH`, THM sends `SIGSTOP`.
- *Real-World Impact*: Freezing the Android emulator breaks ADB connectivity, stalls guest kernel timers, and disconnects active debugger sessions.

== Issue HC-02: Dev Server Watchers & Intermittent Compilers
- *Symptom*: `vite`, `webpack-dev-server`, and `tsc --watch` sleep on file-system inotify events between user saves, showing 0% CPU.
- *Root Cause*: `vite` and `tsc` are absent from `ExecutionDetector::is_known_build_command()`. After 600ms of file inactivity, they flip to `IDLE` and become eligible for memory throttling.
- *Real-World Impact*: Hot module reloading (HMR) responsiveness is delayed when returning to browser.

== Issue HC-03: Cold Discovery Workspace Bias
- *Symptom*: When background workers spawn without a top-level window, they are tagged with the workspace of whatever application the user is currently looking at.
- *Root Cause*: In `daemon.cpp:220`: `int ws_id = ws_monitor.focused_workspace(); wl.workspace_id = ws_id;`.
- *Real-World Impact*: Background daemons spawned while looking at WS7 are assigned `workspace_id = 7`, distorting workspace-level telemetry.

== Issue HC-04: Dynamic Test Runners with Sleeping Intervals
- *Symptom*: `pytest` and `cargo test` suites executing tests with `time.sleep()` or waiting on network mocks drop CPU to 0%.
- *Root Cause*: Python is not a recognized build command and lacks native C++ AST signals in `FusionClassifier`.
- *Real-World Impact*: A long-running test suite could be frozen mid-execution if memory pressure spikes while a test is sleeping.

== Issue HC-05: Browser WebSocket Severing on Freeze
- *Symptom*: Background browser tabs frozen via `SIGSTOP` under high memory pressure lose real-time server connections.
- *Root Cause*: Enforcement applies `signal_tree(wl.pids, SIGSTOP)` to all renderers and network utility processes.
- *Real-World Impact*: WebSockets for Slack, Google Meet, and Discord disconnect after TCP keepalive timeouts.

== Issue HC-06: WebRTC & Media Lack First-Class Latency Recognition in Browsers
- *Symptom*: A user in a Google Meet or Discord video call switches to compile code on WS2; video frames drop.
- *Root Cause*: Chrome is not in `latency_sensitive` (only `obs`, `ffmpeg`, `titan-mirror` are listed). Chrome runs in `archtitan-background.slice` (`cpu.weight=50`) where compilation competes directly with AV1/VP9 encoding.
- *Real-World Impact*: Degraded video and audio packet loss during concurrent compilation.

== Issue HC-07: Headless Containers Lack Dedicated Workload Type
- *Symptom*: Rootless Docker/Podman microservices are categorized as `NEUTRAL` or `CASUAL`.
- *Root Cause*: `WorkloadType` lacks a dedicated `SERVICE` or `CONTAINER` enum variant.
- *Real-World Impact*: Headless database containers may be treated as disposable casual applications.

== Issue HC-08: Virtual Machine Idle Freezing
- *Symptom*: A QEMU/KVM virtual machine waiting on guest input is frozen under high RAM pressure.
- *Root Cause*: QEMU host CPU drops below 5.0% when the guest OS is at its desktop.
- *Real-World Impact*: Network SSH connections to the guest VM stall.

== Issue HC-09: High-Pressure Rapid Workspace Oscillation Signal Churn
- *Symptom*: Rapidly toggling between workspaces under `PressureLevel::HIGH` issues repeated `SIGSTOP` and `SIGCONT` signals.
- *Root Cause*: Workspace state changes immediately upgrade `IDLE -> ACTIVE` and downgrade `ACTIVE -> IDLE` without a minimum hysteresis hold time.
- *Real-World Impact*: Unnecessary kernel signal delivery overhead.

== Issue HC-10: Immutable WorkloadType Assignment
- *Symptom*: A terminal starting `npm run build` and later switching to `cargo build` remains classified as `WEB_DEV`.
- *Root Cause*: `daemon.cpp:216` checks `if (ownership.workload_of(pid) != 0) continue;`, permanently freezing initial semantic classification.
- *Real-World Impact*: Semantic classification does not adapt when the user pivots project types in an existing terminal.

== Issue HC-11: Crash Recovery Gap (Missing Startup Thaw Sweep)
- *Symptom*: If THM v3 crashes (or receives `SIGKILL`) while workloads are frozen, restarted THM does not unfreeze them.
- *Root Cause*: `daemon.cpp` only thaws frozen processes during *clean shutdown* (`lines 327-333`). There is no startup sweep across `/proc` for processes in state `'T'`.
- *Real-World Impact*: After an unexpected THM crash, frozen IDEs and tools remain permanently frozen until the user manually issues `pkill -CONT`.

= The Nightmare Benchmark Results (TC-38 & TC-39)

The headline benchmarks were executed under real process loads:

#grid(
  columns: (1fr, 1fr),
  gutter: 12pt,
  block(fill: rgb("#f8fafc"), stroke: 1pt + rgb("#cbd5e1"), inset: 10pt, radius: 4pt)[
    #text(weight: "bold", size: 10pt, fill: rgb("#0369a1"))[TC-38: 10-Workspace Monster]
    - *Workspaces*: 10 Concurrent Desktops
    - *Workloads*: 10 Polyglot stacks simultaneously active (Web, Android, Kernel, Docker, AI, VM, Browser, Mirror, Data, Mixed)
    - *Result*: *100% Invariant Adherence*
    - *Observations*: Foreground UI prioritized; background Gradle and Cargo builds uninterrupted; idle research throttled; TitanMirror immune.
  ],
  block(fill: rgb("#f8fafc"), stroke: 1pt + rgb("#cbd5e1"), inset: 10pt, radius: 4pt)[
    #text(weight: "bold", size: 10pt, fill: rgb("#0369a1"))[TC-39: 'Everything is on Fire']
    - *Stressors*: RAM > 90%, CPU saturation, active workers, PipeWire audio, stale orphan candidate
    - *Result*: *Zero Interruption*
    - *Observations*:
      - Build interruption: *0*
      - AI execution interruption: *0*
      - Titan ecosystem interruption: *0*
      - Audio playback interruption: *0*
      - Memory recovered: *Measurable* (stale reclaimed via 9-step sequence)
  ]
)

= Recommended Roadmap for THM v3.1

Based on the 11 identified failure-oriented issues, the following targeted refinements are recommended for the next THM development cycle:
1. *Add Startup Thaw Sweep* (`daemon.cpp`): Scan `/proc` on launch for processes in state `'T'` belonging to user UID and issue `SIGCONT` to recover from previous crashes.
2. *Introduce `SERVICE` / `VM` Workload Types*: Exempt background emulators and virtual machines from aggressive freezing based solely on host CPU deltas.
3. *Browser Tab-Level Freezing vs Process Freezing*: Avoid `SIGSTOP` on the main browser parent to keep WebSockets alive.
4. *Dynamic Re-classification*: Allow terminals to re-evaluate `WorkloadType` when child command trees fundamentally change.
5. *Workspace Return Hysteresis*: Add a 2-second debounce window before downgrading active workloads under high memory pressure.
