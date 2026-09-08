# Titan Hardware Manager v3 (THM v3) — Test System & Verification Engine

[![License: GPL-3.0](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
[![Platform: ArchTitan Linux](https://img.shields.io/badge/Platform-ArchTitan%20Linux%20%7C%20Hyprland-cyan.svg)](https://github.com/GitGuru29/archtitan-os)
[![C++20: Ready](https://img.shields.io/badge/C%2B%2B-20%20Standard-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![Core OS Tests: 10/10 PASS](https://img.shields.io/badge/Core%20Tests-10%2F10%20PASS-brightgreen.svg)](#suite-1-core-os-architectural-tests)
[![Hardcore Matrix: 39 Scenarios](https://img.shields.io/badge/Hardcore%20Matrix-39%20Scenarios-orange.svg)](#suite-2-hardcore-real-world-failure-oriented-matrix)

This repository contains the standalone **Test System, Failure-Oriented Verification Engine, and Auditable Decision Pipeline** for **Titan Hardware Manager v3 (THM v3)** on ArchTitan Linux.

---

## 🏛️ Architectural Overview

Titan Hardware Manager v3 establishes a paradigm shift from brittle profile-based resource allocation to an **execution-first, workload-centric model**:

```
Previous (v2):  Workspace ID (Static) ──► Hard Profile ──► Indiscriminate Throttling/Killing
THM v3:         Real-time Activity    ──► Workload State ──► Policy Engine ──► cgroup / POSIX Enforcement
```

### The 5 Core Invariants
1. **Execution Authority**: Real-time process work (`ActivityState::EXECUTING`) is the primary authority. Active background compilation toolchains (`gradle`, `cargo`, `cmake`, `ninja`) are never frozen or terminated.
2. **Workspace Priority Hint**: Workspace visibility (`is_focused` / `is_visible`) serves solely as an interactive scheduling hint (`KEEP_FULL`).
3. **Protected Ecosystem Immunity**: System daemons (`titan-ai`, `titan-share`, `titan-mirror`, `titan-shield`, `titan-gpu`, `pipewire`, `wireplumber`, `Hyprland`, `sddm`, `systemd`) are permanently immune to signals, demotions, or kills.
4. **Demand-Driven AI**: AI tasks run unconstrained during active inference, but have idle memory context paged/frozen under severe memory pressure (>85% RAM).
5. **Anti-Spoofing Verification**: Rogue processes cannot claim immunity by spoofing `comm` strings. Canonical binaries are verified via `/proc/<pid>/exe` against trusted system directories.

---

## 🧪 Test Suite Hierarchy

The test system is organized into two complementary validation suites:

### Suite 1: Core OS Architectural Tests (`thm_test_runner`)
Validates fundamental kernel primitives, POSIX signals, and decoupled policy transitions:
* **TC-1**: Gradle / Background Build Survival (`KEEP_BACKGROUND`)
* **TC-2**: AI Agent Workload Survival & Execution Authority
* **TC-3**: Idle IDE Aging, Throttling & Freeze under Pressure (`SIGSTOP`/`SIGCONT`)
* **TC-4**: Protected Domain Absolute Immunity under Critical Pressure
* **TC-5**: Audio Whitelist & Binary Path Anti-Spoofing
* **TC-6**: Cold-Start Bootstrap & CWD Project Marker Resolution
* **TC-7**: Multi-Workspace Concurrent Active Workloads
* **TC-8**: Elimination of Static Workspace Profiles (Invariance across WS 1..99)
* **TC-9**: 9-Step Reclaim Engine Safety Sequence
* **TC-10**: Demand-Based Dynamic Governor Transitions
* **Result**: **10 / 10 PASS (100% Success)**

---

### Suite 2: Hardcore Real-World Failure-Oriented Matrix (`thm_hardcore_runner`)
Evaluates **39 complex real-world developer workstation scenarios** across **13 groups** (Groups A to M) simulating a 16 GB machine running 10 polyglot workspaces:

| Group | Category | Focus |
| :--- | :--- | :--- |
| **Group A** | Real Workstation Chaos | Multi-stack development (Web + Android + Kernel), 10-workspace monster |
| **Group B** | Break-Point Execution | Variable CPU, I/O waits, deep AI toolchain hierarchies, dormant AI |
| **Group C** | Browser & WebRTC | 40 tabs under memory pressure, Google Meet / video call latency |
| **Group D** | Docker & Daemons | Multi-container microservices, database persistence |
| **Group E** | Virtual Machines | QEMU/KVM 8GB guest VMs, Android emulators |
| **Group F** | Titan Ecosystem | TitanMirror, TitanShare, Titan Shield sandboxes, Titan AI |
| **Group G** | Audio & Bluetooth | PipeWire music streaming, BlueZ peripheral flood |
| **Group H** | RAM Torture | 14–15 GB RAM saturation, final link stage builds, AI compiler launches |
| **Group I** | Race Conditions | Process resumption during 2s grace, new children appearance, workspace switches |
| **Group J** | Process Lifecycle | Application closes while child continues, reparenting to PID 1, context shifts |
| **Group K** | Topology Abuse | Workspace deletion/reuse, cross-workspace workloads, polyglot desktops |
| **Group L** | Crash Recovery | Late THM start, daemon crash during compilation, crash while frozen |
| **Group M** | Nightmare Benchmarks | **TC-38** (10 Workspaces simultaneous load) & **TC-39** ("Everything is on fire") |

* **Scorecard**: **28 PASS** | **10 CONDITIONAL PASS (Edge-Case Identified)** | **1 FAIL (Crash Recovery Thaw Gap)**

---

## 🔍 All 11 Identified Issues & Edge Cases

Running the 39 failure-oriented scenarios on the unmodified engine surfaced 11 specific architectural gaps:

1. **ISSUE-HC-01 (Headless Emulators & DBs)**: Background Android Emulators and headless databases (Redis/Postgres) with <5% CPU risk `FREEZE` under high pressure.
2. **ISSUE-HC-02 (Dev Server Watchers)**: `vite` and `tsc --watch` sleeping on inotify drop to 0% CPU and flip to `IDLE` after 600ms.
3. **ISSUE-HC-03 (Cold Discovery Workspace Bias)**: Background workers spawned without a top-level window are tagged with the user's currently focused workspace.
4. **ISSUE-HC-04 (Dynamic Test Runners)**: `pytest` suites sleeping during tests lack static build keywords and risk being treated as idle.
5. **ISSUE-HC-05 (Browser WebSocket Severing)**: Full `SIGSTOP` on background browser renderers drops WebSockets (Slack, Meet, Discord).
6. **ISSUE-HC-06 (WebRTC Latency Flag Missing in Chrome)**: Chrome is not flagged `latency_sensitive`, causing video packet jitter when heavy compilation runs in the same slice.
7. **ISSUE-HC-07 (Headless Container Categorization)**: Microservices lack a dedicated `SERVICE` / `CONTAINER` workload type.
8. **ISSUE-HC-08 (Virtual Machine Idle Freezing)**: QEMU VMs idling at desktop drop host CPU below 5% and risk being frozen under high memory pressure.
9. **ISSUE-HC-09 (Rapid Workspace Oscillation Signal Churn)**: Toggling workspaces under high pressure without hysteresis generates repeated `SIGSTOP`/`SIGCONT` cycles.
10. **ISSUE-HC-10 (Immutable WorkloadType)**: A long-lived terminal changing projects from `npm` to `cargo` retains its initial `WEB_DEV` semantic tag.
11. **ISSUE-HC-11 (Crash Recovery Gap / Missing Startup Thaw Sweep)**: If THM crashes while processes are frozen, restarting THM does not automatically thaw orphaned `'T'` states.

---

## 📜 Auditable Decision Log Trace

Every decision during test execution is logged into `thm_hardcore_audit_trail.log` with timestamp, workspace ID, workload name, state, CPU%, RSS, pressure level, decision, cgroup slice, and signal:

```text
[22:53:35] WS2 | Gradle             | BG_EXEC      | CPU:85% | RSS:1200MB | P:NORMAL   | DEC:KEEP_BACKGROUND | SLICE:archtitan-bg    | SIG:NONE    | Background build uninterrupted when user switches to WS1
[22:53:35] WS1 | Web-Vite           | BG_EXEC      | CPU:45% | RSS:450MB  | P:NORMAL   | DEC:KEEP_FULL       | SLICE:archtitan-act   | SIG:NONE    | Active focused workspace workload gets full priority
[22:53:35] WS2 | Disk-I/O-Rustc     | EXECUTING    | CPU:0%  | RSS:800MB  | P:NORMAL   | DEC:KEEP_BACKGROUND | SLICE:archtitan-bg    | SIG:NONE    | Kernel D-state preserves execution flag despite 0% CPU
[22:53:35] WS2 | Net-Wait-Curl      | IDLE         | CPU:0%  | RSS:50MB   | P:HIGH     | DEC:FREEZE          | SLICE:archtitan-fzn   | SIG:SIGSTOP | Network wait in S-state lacks command whitelist match; falls to IDLE
[22:53:35] WS3 | Idle-Helper        | IDLE         | CPU:0%  | RSS:400MB  | P:CRITICAL | DEC:FREEZE          | SLICE:archtitan-fzn   | SIG:SIGSTOP | Idle processes frozen to liberate RAM for active agents
[22:53:35] WS8 | TitanMirror        | ACTIVE       | CPU:12% | RSS:180MB  | P:CRITICAL | DEC:KEEP_FULL       | SLICE:archtitan-act   | SIG:NONE    | TitanMirror hard-block protected: immune from freeze and cgroup demotion
[22:53:37] WS3 | Resumed-Worker     | RECLAIMABLE  | CPU:25% | RSS:150MB  | P:NORMAL   | DEC:ABORT_RECLAIM   | SLICE:archtitan-bg    | SIG:NONE    | Reclaim aborted during grace period because activity resumed
[22:53:42] WS2 | Fire-Build         | BG_EXEC      | CPU:99% | RSS:3200MB | P:CRITICAL | DEC:KEEP_BACKGROUND | SLICE:archtitan-bg    | SIG:NONE    | Build survives RAM>90% and CPU saturation
[22:53:42] WS0 | Fire-Audio         | ACTIVE       | CPU:4%  | RSS:110MB  | P:CRITICAL | DEC:KEEP_FULL       | SLICE:archtitan-act   | SIG:NONE    | PipeWire audio immune during system-wide fire
[22:53:42] WS7 | Fire-Stale         | RECLAIMABLE  | CPU:0%  | RSS:220MB  | P:CRITICAL | DEC:RECLAIM         | SLICE:archtitan-fzn   | SIG:SIGTERM | Stale memory liberated under extreme pressure
```

---

## 🛠️ Build and Run Instructions

### Prerequisites
* Linux Kernel 6.x with cgroup v2 unified hierarchy
* GCC / G++ 11+ (supporting C++20)
* CMake 3.18+
* `typst` (for generating PDF technical reports)

### 1. Clone Repository
```bash
git clone https://github.com/GitGuru29/titan-hardware-manager-test-system.git
cd titan-hardware-manager-test-system
```

### 2. Configure & Build
```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### 3. Run Test Suites
```bash
# Run Core Architectural Test Suite (10 Tests)
./thm_test_runner

# Run Hardcore Real-World Matrix (39 Scenarios)
./thm_hardcore_runner

# Run all via CTest
ctest --verbose
```

### 4. Compile Typst Technical Reports
```bash
cd ../docs
typst compile thm_v3_architecture_report.typ thm_v3_architecture_report.pdf
typst compile thm_v3_hardcore_test_report.typ thm_v3_hardcore_test_report.pdf
```

---

## 📄 Documentation Artifacts

* [`docs/thm_v3_architecture_report.pdf`](docs/thm_v3_architecture_report.pdf): Publication-grade architecture whitepaper (7 pages).
* [`docs/thm_v3_hardcore_test_report.pdf`](docs/thm_v3_hardcore_test_report.pdf): Full 39-scenario test report & failure analysis (5 pages).
* [`thm_hardcore_audit_trail.log`](thm_hardcore_audit_trail.log): Complete auditable decision log.

---

## ⚖️ License
GNU General Public License v3.0 (GPL-3.0) - ArchTitan OS Engineering.
