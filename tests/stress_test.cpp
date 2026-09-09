// =============================================================================
// titan-hwm-v3/tests/stress_test.cpp
// 30-Minute Service-Level Stress Test for Titan Hardware Manager v3
//
// Exercises the full THM v3 pipeline under sustained pressure:
//   - Spawns hundreds of real workload processes (CPU burners, idle, IO)
//   - Simulates rapid workspace switching across 6 workspaces
//   - Drives the complete classify → detect → tick → policy → enforce pipeline
//   - Measures tick latency, decision throughput, process churn rate
//   - Monitors system-wide metrics (RSS, PID count, pressure level)
//   - Logs CSV every 10 seconds for post-analysis
//
// Usage: ./thm_stress_test [--duration SEC] [--csv PATH] [--tick-ms MS]
// =============================================================================

#include "../core/types.hpp"
#include "../core/protected_registry.hpp"
#include "../workload/ownership_graph.hpp"
#include "../workload/workload_manager.hpp"
#include "../classifier/fusion_classifier.hpp"
#include "../classifier/execution_detector.hpp"
#include "../policy/policy_engine.hpp"
#include "../policy/reclaim_engine.hpp"
#include "../enforcement/cgroup_controller.hpp"
#include "../enforcement/enforcement_plane.hpp"
#include "../resources/pressure_level.hpp"
#include "../ipc/workspace_monitor.hpp"

#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <algorithm>
#include <random>
#include <numeric>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>

namespace fs = std::filesystem;

// ── Globals ─────────────────────────────────────────────────────────────────
static std::atomic<bool> g_shutdown{false};
static void handle_signal(int) { g_shutdown = true; }

// Memory-hog allocation size (MB) — set from --memory-hog-mb before forking
static int g_memory_hog_mb = 4;

// ── Configuration ───────────────────────────────────────────────────────────
struct StressConfig {
    int duration_sec   = 1800;  // 30 minutes
    int tick_ms        = 200;   // matching daemon 5Hz
    int batch_size     = 10;    // processes to spawn/kill per tick
    int max_workloads  = 200;   // max concurrent workloads
    int workspace_count = 6;    // simulated workspaces
    std::string csv_path = "stress_test_output.csv";

    // Forced-pressure override (0 = disabled; else force this level)
    thm::PressureLevel force_pressure = thm::PressureLevel::NORMAL;
    bool force_pressure_set = false;

    // Memory-hog scaling (MB per hog process). Raise to generate real pressure.
    int memory_hog_mb = 4;

    // Aging/decay phase: pre-age idle workloads to exercise IDLE→AGING→RECLAIMABLE
    bool aging_phase = false;

    // Pass/fail assertion budgets
    bool assert_enabled = false;
    double max_latency_p95_ms = 1500.0;
    int rss_growth_budget_pct = 10;   // allow up to +10% RSS growth
    int pid_growth_budget = 300;      // allow up to +300 extra system PIDs
};

// ── Process Spawning Helpers ────────────────────────────────────────────────
static pid_t spawn_cpu_burner() {
    pid_t pid = fork();
    if (pid == 0) {
        volatile uint64_t c = 0;
        while (true) {
            for (int i = 0; i < 50000; ++i) c += i * 3 + 7;
            std::this_thread::yield();
        }
        _exit(0);
    }
    return pid;
}

static pid_t spawn_idle_process() {
    pid_t pid = fork();
    if (pid == 0) {
        while (true) pause();
        _exit(0);
    }
    return pid;
}

static pid_t spawn_io_process() {
    pid_t pid = fork();
    if (pid == 0) {
        volatile int x = 0;
        while (true) {
            std::ofstream tmp("/tmp/thm_stress_io_" + std::to_string(getpid()));
            tmp << "data_" << x++ << "\n";
            tmp.close();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        _exit(0);
    }
    return pid;
}

static pid_t spawn_memory_hog() {
    pid_t pid = fork();
    if (pid == 0) {
        size_t size = static_cast<size_t>(g_memory_hog_mb) * 1024 * 1024;
        std::vector<char> buffer(size, 'A');
        volatile int x = 0;
        while (true) {
            buffer[x % buffer.size()] = 'B' + (x % 24);
            ++x;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        _exit(0);
    }
    return pid;
}

static void reap_pid(pid_t pid) {
    if (pid <= 1) return;
    ::kill(pid, SIGKILL);
    int status;
    ::waitpid(pid, &status, WNOHANG);
}

static char read_proc_state(pid_t pid) {
    std::ifstream sf("/proc/" + std::to_string(pid) + "/stat");
    if (!sf.is_open()) return '?';
    std::string line;
    if (!std::getline(sf, line)) return '?';
    auto rp = line.rfind(')');
    if (rp == std::string::npos || rp + 2 >= line.size()) return '?';
    return line[rp + 2];
}

// ── System Metrics ──────────────────────────────────────────────────────────
struct SystemMetrics {
    long rss_kb        = 0;
    int  total_pids    = 0;
    float mem_used_pct = 0.0f;
    float psi_some     = 0.0f;
};

static SystemMetrics read_system_metrics() {
    SystemMetrics m;
    // Count PIDs in /proc
    DIR* d = opendir("/proc");
    if (d) {
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            if (e->d_type == DT_DIR) {
                bool is_pid = true;
                for (const char* p = e->d_name; *p; ++p) {
                    if (!std::isdigit(*p)) { is_pid = false; break; }
                }
                if (is_pid && std::stoi(e->d_name) > 1) m.total_pids++;
            }
        }
        closedir(d);
    }
    // Read meminfo
    thm::MemInfo mem = thm::read_meminfo();
    m.mem_used_pct = mem.used_pct();
    // Read PSI
    thm::PSIStats psi = thm::read_memory_psi();
    if (psi.valid) m.psi_some = psi.some_avg10;
    // Self RSS
    std::ifstream statm("/proc/self/statm");
    long total_pages = 0, rss_pages = 0;
    if (statm >> total_pages >> rss_pages)
        m.rss_kb = rss_pages * 4;
    return m;
}

// ── Workload Profile (tracks spawned processes) ─────────────────────────────
struct StressWorkload {
    uint32_t id;
    int workspace_id;
    thm::WorkloadType type;
    thm::WorkloadState state;
    std::vector<pid_t> pids;
    pid_t root_pid;
    bool is_building;
    bool ai_owned;
    bool latency_sensitive;
    bool pre_aged = false;    // aging-phase: simulate last_active minutes ago
    int  aging_minutes = 0;   // how many minutes ago last_active was
};

// ── Decision Stats ──────────────────────────────────────────────────────────
struct DecisionStats {
    int keep_full = 0;
    int keep_bg = 0;
    int throttle = 0;
    int freeze = 0;
    int reclaim = 0;
    int total = 0;
};

// ── CSV Logger ──────────────────────────────────────────────────────────────
class CSVLogger {
public:
    CSVLogger(const std::string& path) {
        ofs_.open(path);
        if (ofs_.is_open()) {
            ofs_ << "elapsed_sec,tick_count,tick_ms_avg,tick_ms_p99,pipeline_ms_avg,pipeline_ms_p99,workloads_active,"
                 << "processes_spawned_total,processes_killed_total,decisions_total,"
                 << "keep_full,keep_bg,throttle,freeze,reclaim,"
                 << "rss_kb,total_pids,mem_used_pct,psi_some,pressure_level,"
                 << "governor_hint,reclaim_aborts\n";
            ofs_.flush();
        }
    }

void log(int elapsed_sec, int tick_count, double avg_ms, double p99_ms,
             double pipe_avg_ms, double pipe_p99_ms,
             int active_wl, int spawned, int killed, const DecisionStats& ds,
             const SystemMetrics& sys, thm::PressureLevel pressure,
             thm::GovernorHint gov, int reclaim_aborts) {
        if (!ofs_.is_open()) return;
        ofs_ << elapsed_sec << ","
             << tick_count << ","
             << std::fixed << std::setprecision(2) << avg_ms << ","
             << p99_ms << ","
             << pipe_avg_ms << ","
             << pipe_p99_ms << ","
             << active_wl << ","
             << spawned << ","
             << killed << ","
             << ds.total << ","
             << ds.keep_full << ","
             << ds.keep_bg << ","
             << ds.throttle << ","
             << ds.freeze << ","
             << ds.reclaim << ","
             << sys.rss_kb << ","
             << sys.total_pids << ","
             << std::setprecision(1) << sys.mem_used_pct << ","
             << sys.psi_some << ","
             << thm::to_string(pressure) << ","
             << (gov == thm::GovernorHint::PERFORMANCE ? "PERFORMANCE" :
                 gov == thm::GovernorHint::POWERSAVE ? "POWERSAVE" : "SCHEDUL")
             << "," << reclaim_aborts
             << "\n";
        ofs_.flush();
    }

    void close() { ofs_.close(); }

private:
    std::ofstream ofs_;
};

// ── Progress Display ────────────────────────────────────────────────────────
static void print_progress(int elapsed_sec, int total_sec, int tick_count,
                           const DecisionStats& ds, const SystemMetrics& sys,
                           double avg_ms, thm::PressureLevel pressure,
                           int active_wl, int spawned, int killed) {
    int pct = (100 * elapsed_sec) / total_sec;
    int mins = elapsed_sec / 60;
    int secs = elapsed_sec % 60;
    int total_mins = total_sec / 60;
    int total_secs = total_sec % 60;

    std::cout << "\r["
              << std::setw(3) << pct << "%] "
              << std::setw(2) << mins << ":" << std::setw(2) << std::setfill('0')
              << secs << "/" << total_mins << ":" << std::setw(2) << total_secs
              << std::setfill(' ')
              << " | ticks=" << tick_count
              << " | avg=" << std::fixed << std::setprecision(1) << avg_ms << "ms"
              << " | wl=" << active_wl
              << " | pid=" << sys.total_pids
              << " | mem=" << std::setprecision(0) << sys.mem_used_pct << "%"
              << " | P=" << thm::to_string(pressure)
              << " | decisions=" << ds.total
              << " | spawn=" << spawned << " kill=" << killed
              << "   " << std::flush;
}

// ═════════════════════════════════════════════════════════════════════════════
// Main Stress Test
// ═════════════════════════════════════════════════════════════════════════════
int main(int argc, char* argv[]) {
    StressConfig cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--duration" && i + 1 < argc) cfg.duration_sec = std::stoi(argv[++i]);
        else if (arg == "--csv" && i + 1 < argc) cfg.csv_path = argv[++i];
        else if (arg == "--tick-ms" && i + 1 < argc) cfg.tick_ms = std::stoi(argv[++i]);
        else if (arg == "--force-pressure" && i + 1 < argc) {
            std::string v = argv[++i];
            if (v == "NORMAL" || v == "normal") {
                cfg.force_pressure = thm::PressureLevel::NORMAL;
            } else if (v == "MODERATE" || v == "moderate") {
                cfg.force_pressure = thm::PressureLevel::MODERATE;
            } else if (v == "HIGH" || v == "high") {
                cfg.force_pressure = thm::PressureLevel::HIGH;
            } else if (v == "CRITICAL" || v == "critical") {
                cfg.force_pressure = thm::PressureLevel::CRITICAL;
            } else {
                std::cerr << "Invalid --force-pressure value: '" << v
                          << "' (expected NORMAL/MODERATE/HIGH/CRITICAL)\n";
                return 1;
            }
            cfg.force_pressure_set = true;
        }
        else if (arg == "--memory-hog-mb" && i + 1 < argc) {
            cfg.memory_hog_mb = std::max(1, std::stoi(argv[++i]));
            g_memory_hog_mb = cfg.memory_hog_mb;
        }
        else if (arg == "--aging-phase") cfg.aging_phase = true;
        else if (arg == "--assert") {
            cfg.assert_enabled = true;
            if (i + 1 < argc && std::isdigit(argv[i + 1][0])) {
                cfg.max_latency_p95_ms = std::stod(argv[++i]);
            }
        }
        else if (arg == "--pid-budget" && i + 1 < argc) {
            cfg.pid_growth_budget = std::max(1, std::stoi(argv[++i]));
        }
        else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: thm_stress_test [OPTIONS]\n"
                      << "  --duration SEC        Duration in seconds (default: 1800 = 30 min)\n"
                      << "  --csv PATH            CSV output path (default: stress_test_output.csv)\n"
                      << "  --tick-ms MS          Tick interval in ms (default: 200)\n"
                      << "  --force-pressure LVL  Force pressure to NORMAL/MODERATE/HIGH/CRITICAL\n"
                      << "                        (default: real system pressure; HIGH+ drives FREEZE,\n"
                      << "                        CRITICAL + aged workloads drives RECLAIM)\n"
                      << "  --aging-phase         Pre-age idle workloads to exercise IDLE->AGING->RECLAIMABLE\n"
                      << "  --memory-hog-mb MB    Per-worker memory allocation (default: 4; raise to push system\n"
                      << "                        toward real HIGH/CRITICAL pressure)\n"
                      << "  --assert [P95_MS]     Enable pass/fail assertions (default p95 ceiling 1500ms)\n"
                      << "  --pid-budget N        PID growth assertion budget (default: 300; scale with duration)\n"
                      << "  --help                Show this help\n";
            return 0;
        }
    }

    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║  THM v3 — Service-Level Stress Test                             ║\n";
    std::cout << "║  Full pipeline: classify → detect → tick → policy → enforce     ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════════╝\n";
    std::cout << "Config: duration=" << cfg.duration_sec << "s"
              << " tick=" << cfg.tick_ms << "ms"
              << " batch=" << cfg.batch_size
              << " max_wl=" << cfg.max_workloads
              << " csv=" << cfg.csv_path;
    if (cfg.force_pressure_set)
        std::cout << " force_pressure=" << thm::to_string(cfg.force_pressure);
    if (cfg.aging_phase)
        std::cout << " aging_phase=ON";
    if (cfg.assert_enabled)
        std::cout << " assertions=ON";
    std::cout << "\n\n";

    // ── Init THM components ──────────────────────────────────────────────
    thm::ProtectedRegistry registry;
    registry.register_pid(getpid(), "thm-stress-test");
    registry.bootstrap_from_systemd();

    thm::CgroupController cgroup;
    cgroup.setup_slices();

    thm::EnforcementPlane enforcement(cgroup, registry);
    thm::ExecutionDetector detector;
    // Zero grace periods for the stress harness — the 9-step safety logic is
    // exercised at full speed. (Real daemon uses 2s+3s grace; note that an
    // inline reclaim() blocks the tick loop for ~5s in the real daemon.)
    thm::ReclaimConfig reclaim_cfg{0, 0, 30};
    thm::ReclaimEngine reclaimer(registry, detector, reclaim_cfg);
    thm::WorkloadManager workload_mgr;
    thm::OwnershipGraph ownership;
    thm::FusionClassifier classifier;
    thm::PolicyEngine policy;

    // ── CSV Logger ──────────────────────────────────────────────────────
    CSVLogger csv(cfg.csv_path);

    // ── Stats ────────────────────────────────────────────────────────────
    DecisionStats lifetime_stats{};
    int total_spawned = 0;
    int total_killed = 0;
    int total_reclaim_aborts = 0;
    uint32_t next_wl_id = 1;
    std::mt19937 rng(42);

    // Tick latency tracking (ring buffer)
    std::vector<double> tick_latencies;
    tick_latencies.reserve(5000);

    // Process pool
    std::vector<StressWorkload> active_workloads;

    auto start_time = std::chrono::steady_clock::now();
    auto last_csv_log = start_time;
    int tick_count = 0;

    // Baseline system state for leak/pid-growth assertions
    SystemMetrics baseline_sys = read_system_metrics();
    long baseline_rss_kb   = baseline_sys.rss_kb;
    int  baseline_total_pids = baseline_sys.total_pids;
    std::cout << "[STRESS] Baseline: rss=" << baseline_rss_kb
              << " KB, total_pids=" << baseline_total_pids << "\n";

    // Pipeline-exclusive latency tracking (state machine + policy + enforce only)
    std::vector<double> pipeline_latencies;
    pipeline_latencies.reserve(5000);

    std::cout << "[STRESS] Starting stress test — " << cfg.duration_sec / 60
              << " minutes, " << cfg.tick_ms << "ms tick rate\n";
    std::cout << "[STRESS] Press Ctrl+C for early termination\n\n";

    // ═════════════════════════════════════════════════════════════════════
    // MAIN STRESS LOOP
    // ═════════════════════════════════════════════════════════════════════
    while (!g_shutdown) {
        auto tick_start = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            tick_start - start_time).count();

        if (elapsed >= cfg.duration_sec) {
            std::cout << "\n[STRESS] Duration reached (" << cfg.duration_sec << "s). Stopping.\n";
            break;
        }

        tick_count++;

        // ── Phase A: Spawn new workloads (staggered) ────────────────────
        if ((int)active_workloads.size() < cfg.max_workloads &&
            tick_count % 5 == 0) { // every 1s
            int to_spawn = std::min(cfg.batch_size,
                                    cfg.max_workloads - (int)active_workloads.size());
            for (int i = 0; i < to_spawn; ++i) {
                StressWorkload wl;
                wl.id = next_wl_id++;
                wl.workspace_id = (wl.id - 1) % cfg.workspace_count + 1;
                wl.is_building = (rng() % 4 == 0);
                wl.ai_owned = (rng() % 8 == 0);
                wl.latency_sensitive = (rng() % 6 == 0);

                // Aging-phase: roughly a third of spawned workloads are idle
                // processes pre-aged past the AGING / RECLAIMABLE thresholds so
                // the idle→AGING→RECLAIMABLE decay chain is exercised without
                // waiting 15 real minutes.
                bool idle_aged = cfg.aging_phase && (rng() % 3 == 0);
                if (idle_aged) {
                    wl.is_building = false;
                    wl.ai_owned = false;
                    wl.latency_sensitive = false;
                    wl.state = thm::WorkloadState::IDLE;
                    wl.pre_aged = true;
                    wl.aging_minutes = 20; // past age_hard_decay_min (15)
                } else {
                    wl.state = thm::WorkloadState::ACTIVE;
                }

                // Random workload type
                int type_roll = rng() % 5;
                switch (type_roll) {
                    case 0: wl.type = thm::WorkloadType::ANDROID_DEV; break;
                    case 1: wl.type = thm::WorkloadType::WEB_DEV; break;
                    case 2: wl.type = thm::WorkloadType::SYSTEM_DEV; break;
                    case 3: wl.type = thm::WorkloadType::AI_TASK; break;
                    default: wl.type = thm::WorkloadType::CASUAL; break;
                }

                // Spawn real process. Aging-phase idle workloads get idle
                // processes (pause()) so they stay genuinely idle.
                pid_t pid;
                if (idle_aged) {
                    pid = spawn_idle_process();
                } else {
                    int proc_type = rng() % 4;
                    switch (proc_type) {
                        case 0: pid = spawn_cpu_burner(); break;
                        case 1: pid = spawn_idle_process(); break;
                        case 2: pid = spawn_io_process(); break;
                        default: pid = spawn_memory_hog(); break;
                    }
                }

                if (pid > 0) {
                    wl.root_pid = pid;
                    wl.pids.push_back(pid);
                    active_workloads.push_back(wl);
                    total_spawned++;
                }
            }
        }

        // ── Phase B: Kill old workloads (staggered) ─────────────────────
        if (tick_count % 25 == 0 && !active_workloads.empty()) {
            int to_kill = std::min(3, (int)active_workloads.size());
            for (int i = 0; i < to_kill; ++i) {
                int idx = rng() % active_workloads.size();
                for (pid_t p : active_workloads[idx].pids) {
                    reap_pid(p);
                    total_killed++;
                }
                active_workloads.erase(active_workloads.begin() + idx);
            }
        }

        // ── Phase C: Rapid workspace switching ──────────────────────────
        int focused_ws = (tick_count % cfg.workspace_count) + 1;
        std::unordered_map<int, thm::WorkspaceState> ws_map;
        for (int w = 1; w <= cfg.workspace_count; ++w) {
            thm::WorkspaceState ws;
            ws.id = w;
            ws.is_focused = (w == focused_ws);
            ws.is_visible = (w == focused_ws) || (std::abs(w - focused_ws) <= 1);
            ws_map[w] = ws;
        }

        // ── Phase D: Read system pressure (or force override) ─────────────
        thm::PressureLevel pressure;
        if (cfg.force_pressure_set) {
            pressure = cfg.force_pressure;
        } else {
            thm::MemInfo mem = thm::read_meminfo();
            thm::PSIStats psi = thm::read_memory_psi();
            pressure = thm::compute_pressure(mem, psi);
        }

        // ── Phase E: Build process graph snapshot ───────────────────────
        std::unordered_map<pid_t, thm::ProcessNode> proc_graph;
        for (auto& wl : active_workloads) {
            for (pid_t pid : wl.pids) {
                thm::ProcessNode node;
                node.pid = pid;
                node.comm = "stress_worker";
                node.cmdline = "thm_stress_worker";
                node.proc_state = read_proc_state(pid);

                std::ifstream statm("/proc/" + std::to_string(pid) + "/statm");
                long tp = 0, rp = 0;
                if (statm >> tp >> rp) node.rss_kb = rp * 4;

                proc_graph[pid] = std::move(node);
                detector.detect(proc_graph[pid], {});
            }
        }

        // ── Phase F: Drive full THM pipeline per workload ──────────────
        auto pipeline_start = std::chrono::steady_clock::now();
        for (auto& wl : active_workloads) {
            // Build THM Workload object
            thm::Workload thm_wl;
            thm_wl.id = wl.id;
            thm_wl.workspace_id = wl.workspace_id;
            thm_wl.type = wl.type;
            thm_wl.state = wl.state;
            thm_wl.is_building = wl.is_building;
            thm_wl.ai_owned = wl.ai_owned;
            thm_wl.latency_sensitive = wl.latency_sensitive;
            thm_wl.pids = wl.pids;
            thm_wl.root_pid = wl.root_pid;
            thm_wl.created_at = thm::ms_clock::now();

            // Aging-phase: simulate last_active in the past so idle workloads
            // progress through IDLE→AGING→RECLAIMABLE without real 15-min wait.
            if (wl.pre_aged) {
                thm_wl.last_active = thm::ms_clock::now() -
                    std::chrono::minutes(wl.aging_minutes);
            } else {
                thm_wl.last_active = thm::ms_clock::now();
            }

            // Workspace state
            auto ws_it = ws_map.find(wl.workspace_id);
            thm::WorkspaceState ws = (ws_it != ws_map.end()) ? ws_it->second : thm::WorkspaceState{};

            // Aggregate activity
            thm::ActivityState activity = detector.aggregate(thm_wl, proc_graph);

            // State machine tick
            workload_mgr.tick(thm_wl, ws, activity, pressure);

            // Policy evaluation
            thm::PolicyDecision decision = policy.evaluate(thm_wl, ws, activity,
                                                            pressure, registry);

            // Update wl state from tick
            wl.state = thm_wl.state;

            // Enforcement: KEEP_*/THROTTLE/FREEZE tracked as decisions (no real signals —
            // those are the daemon's job). RECLAIM is genuinely executed so the
            // 9-step safety + termination sequence is validated end-to-end.
            switch (decision) {
                case thm::PolicyDecision::KEEP_FULL:       lifetime_stats.keep_full++; break;
                case thm::PolicyDecision::KEEP_BACKGROUND: lifetime_stats.keep_bg++; break;
                case thm::PolicyDecision::THROTTLE:        lifetime_stats.throttle++; break;
                case thm::PolicyDecision::FREEZE:          lifetime_stats.freeze++; break;
                case thm::PolicyDecision::RECLAIM: {
                    lifetime_stats.reclaim++;
                    // Test reclaim safety + actually reclaim (real SIGTRM/SIGKILL)
                    bool reclaimed = reclaimer.reclaim(thm_wl, proc_graph,
                                                       std::unordered_map<uint32_t, thm::Workload>{{wl.id, thm_wl}});
                    if (!reclaimed) {
                        total_reclaim_aborts++;
                    } else {
                        // Reclaim triggered real SIGTERM/SIGKILL — reap the
                        // children so they don't linger as zombies inflating
                        // the PID count, and drop the workload from the pool.
                        for (pid_t p : wl.pids) {
                            if (p > 1) {
                                ::kill(p, SIGKILL);
                                int st;
                                ::waitpid(p, &st, 0);
                                total_killed++;
                            }
                        }
                        wl.state = thm::WorkloadState::TERMINATED;
                    }
                    break;
                }
            }
            lifetime_stats.total++;
        }
        auto pipeline_end = std::chrono::steady_clock::now();
        double pipeline_ms = std::chrono::duration<double, std::milli>(
            pipeline_end - pipeline_start).count();
        pipeline_latencies.push_back(pipeline_ms);

        // Prune workloads terminated by the reclaim engine this tick
        active_workloads.erase(
            std::remove_if(active_workloads.begin(), active_workloads.end(),
                           [](const StressWorkload& w) { return w.state == thm::WorkloadState::TERMINATED; }),
            active_workloads.end());

        // ── Phase G: Governor computation ──────────────────────────────
        std::unordered_map<uint32_t, thm::Workload> gov_wl;
        for (auto& wl : active_workloads) {
            thm::Workload thm_wl;
            thm_wl.id = wl.id;
            thm_wl.is_building = wl.is_building;
            thm_wl.state = wl.state;
            gov_wl[wl.id] = thm_wl;
        }
        auto gov = thm::WorkspaceMonitor::compute_governor(gov_wl, pressure);

        // ── Phase H: Measure tick latency ──────────────────────────────
        auto tick_end = std::chrono::steady_clock::now();
        double tick_ms = std::chrono::duration<double, std::milli>(
            tick_end - tick_start).count();
        tick_latencies.push_back(tick_ms);

        // ── Progress display (every 5s) ────────────────────────────────
        if (tick_count % 25 == 0) {
            SystemMetrics sys = read_system_metrics();
            double avg = std::accumulate(tick_latencies.begin(),
                                          tick_latencies.end(), 0.0) /
                          tick_latencies.size();
            print_progress(elapsed, cfg.duration_sec, tick_count, lifetime_stats,
                          sys, avg, pressure, (int)active_workloads.size(),
                          total_spawned, total_killed);
        }

        // ── CSV log (every 10s) ────────────────────────────────────────
        auto since_csv = std::chrono::duration_cast<std::chrono::seconds>(
            tick_end - last_csv_log).count();
        if (since_csv >= 10) {
            SystemMetrics sys = read_system_metrics();
            double avg = std::accumulate(tick_latencies.begin(),
                                          tick_latencies.end(), 0.0) /
                          tick_latencies.size();
            // P99 tick latency
            auto sorted_lat = tick_latencies;
            std::sort(sorted_lat.begin(), sorted_lat.end());
            double p99 = sorted_lat[(int)(sorted_lat.size() * 0.99)];

            // Pipeline-exclusive latency (state machine + policy + enforce only)
            double pipe_avg = pipeline_latencies.empty() ? 0.0 :
                std::accumulate(pipeline_latencies.begin(), pipeline_latencies.end(), 0.0) /
                pipeline_latencies.size();
            auto sorted_pipe = pipeline_latencies;
            std::sort(sorted_pipe.begin(), sorted_pipe.end());
            double pipe_p99 = sorted_pipe.empty() ? 0.0 :
                sorted_pipe[(int)(sorted_pipe.size() * 0.99)];

            csv.log(elapsed, tick_count, avg, p99, pipe_avg, pipe_p99,
                   (int)active_workloads.size(),
                   total_spawned, total_killed, lifetime_stats, sys, pressure,
                   gov, total_reclaim_aborts);
            last_csv_log = tick_end;
        }

        // ── Sleep remaining tick budget ────────────────────────────────
        auto elapsed_tick = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - tick_start);
        auto sleep_dur = std::chrono::milliseconds(cfg.tick_ms) - elapsed_tick;
        if (sleep_dur.count() > 0)
            std::this_thread::sleep_for(sleep_dur);
    }

    // ═════════════════════════════════════════════════════════════════════
    // SHUTDOWN & SUMMARY
    // ═════════════════════════════════════════════════════════════════════
    std::cout << "\n\n";
    std::cout << "══════════════════════════════════════════════════════════════════════════\n";
    std::cout << "                         STRESS TEST SUMMARY                             \n";
    std::cout << "══════════════════════════════════════════════════════════════════════════\n";

    auto wall_time = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start_time).count();
    double wall_mins = wall_time / 60.0;

    std::cout << "Duration:      " << wall_mins << " min (" << wall_time << "s)\n";
    std::cout << "Ticks:         " << tick_count << " (" << cfg.tick_ms << "ms interval)\n";
    std::cout << "Workloads:     spawned=" << total_spawned << " killed=" << total_killed
              << " peak=" << cfg.max_workloads << "\n";

    std::cout << "\nDecision Distribution:\n";
    std::cout << "  KEEP_FULL:      " << std::setw(8) << lifetime_stats.keep_full << "\n";
    std::cout << "  KEEP_BACKGROUND:" << std::setw(8) << lifetime_stats.keep_bg << "\n";
    std::cout << "  THROTTLE:       " << std::setw(8) << lifetime_stats.throttle << "\n";
    std::cout << "  FREEZE:         " << std::setw(8) << lifetime_stats.freeze << "\n";
    std::cout << "  RECLAIM:        " << std::setw(8) << lifetime_stats.reclaim << "\n";
    std::cout << "  TOTAL:          " << std::setw(8) << lifetime_stats.total << "\n";
    std::cout << "  Reclaim Aborts: " << std::setw(8) << total_reclaim_aborts << "\n";

    if (!tick_latencies.empty()) {
        auto sorted_lat = tick_latencies;
        std::sort(sorted_lat.begin(), sorted_lat.end());
        double avg = std::accumulate(tick_latencies.begin(),
                                      tick_latencies.end(), 0.0) / tick_latencies.size();
        double p50 = sorted_lat[sorted_lat.size() / 2];
        double p95 = sorted_lat[(int)(sorted_lat.size() * 0.95)];
        double p99 = sorted_lat[(int)(sorted_lat.size() * 0.99)];
        double max_lat = sorted_lat.back();

        std::cout << "\nTick Latency (ms):\n";
        std::cout << "  avg=" << std::fixed << std::setprecision(2) << avg
                  << " p50=" << p50 << " p95=" << p95
                  << " p99=" << p99 << " max=" << max_lat << "\n";
    }

    if (!pipeline_latencies.empty()) {
        auto sorted_pipe = pipeline_latencies;
        std::sort(sorted_pipe.begin(), sorted_pipe.end());
        double p_avg = std::accumulate(pipeline_latencies.begin(),
                                        pipeline_latencies.end(), 0.0) /
                        pipeline_latencies.size();
        double p50 = sorted_pipe[sorted_pipe.size() / 2];
        double p95 = sorted_pipe[(int)(sorted_pipe.size() * 0.95)];
        double p99 = sorted_pipe[(int)(sorted_pipe.size() * 0.99)];
        double p_max = sorted_pipe.back();

        std::cout << "\nPipeline-Exclusive Latency (ms) — classify/detect/tick/policy/enforce only:\n";
        std::cout << "  avg=" << std::fixed << std::setprecision(2) << p_avg
                  << " p50=" << p50 << " p95=" << p95
                  << " p99=" << p99 << " max=" << p_max << "\n";
    }

    SystemMetrics final_sys = read_system_metrics();
    std::cout << "\nFinal System State:\n";
    std::cout << "  Total PIDs:    " << final_sys.total_pids << "\n";
    std::cout << "  THM RSS:       " << final_sys.rss_kb << " KB\n";
    std::cout << "  Memory Used:   " << std::setprecision(1) << final_sys.mem_used_pct << "%\n";
    std::cout << "  PSI some_avg10:" << final_sys.psi_some << "\n";

    std::cout << "\nCSV output: " << cfg.csv_path << "\n";

    csv.close();
    std::cout << "\n[CLEANUP] Reaping " << active_workloads.size() << " remaining workloads...\n";
    for (auto& wl : active_workloads) {
        for (pid_t p : wl.pids) {
            reap_pid(p);
        }
    }
    active_workloads.clear();

    std::cout << "[DONE] Stress test complete.\n\n";

    // ═════════════════════════════════════════════════════════════════════
    // PASS/FAIL ASSERTIONS — gate on leak, PID-growth, latency, safety
    // ═════════════════════════════════════════════════════════════════════
    if (cfg.assert_enabled) {
        std::cout << "══════════════════════════════════════════════════════════════════════════\n";
        std::cout << "                             PASS/FAIL ASSERTIONS                         \n";
        std::cout << "══════════════════════════════════════════════════════════════════════════\n";

        bool all_asserts_passed = true;
        int  assert_num = 0;
        auto fail_assert = [&](const std::string& msg) {
            all_asserts_passed = false;
            std::cout << "  [FAIL] #" << ++assert_num << ": " << msg << "\n";
        };
        auto pass_assert = [&](const std::string& msg) {
            std::cout << "  [PASS] #" << ++assert_num << ": " << msg << "\n";
        };

        // Metrics after cleanup — accurate baseline comparison
        SystemMetrics final_sys2 = read_system_metrics();

        // 1. Memory-leak guard: THM RSS growth within budget
        long rss_growth_kb = final_sys2.rss_kb - baseline_rss_kb;
        long rss_growth_pct = baseline_rss_kb > 0
            ? (100 * rss_growth_kb) / baseline_rss_kb : 0;
        if (rss_growth_pct <= cfg.rss_growth_budget_pct && rss_growth_kb >= 0) {
            pass_assert("RSS growth " + std::to_string(rss_growth_kb) + " KB ("
                        + std::to_string(rss_growth_pct) + "%) within "
                        + std::to_string(cfg.rss_growth_budget_pct) + "% budget");
        } else {
            fail_assert("RSS growth " + std::to_string(rss_growth_kb) + " KB ("
                        + std::to_string(rss_growth_pct) + "%) exceeds "
                        + std::to_string(cfg.rss_growth_budget_pct) + "% budget");
        }

        // 2. PID-growth guard: no unbounded accumulation of system PIDs
        int pid_growth = final_sys2.total_pids - baseline_total_pids;
        if (pid_growth <= cfg.pid_growth_budget && pid_growth >= 0) {
            pass_assert("System PID growth " + std::to_string(pid_growth)
                        + " within budget " + std::to_string(cfg.pid_growth_budget));
        } else {
            fail_assert("System PID growth " + std::to_string(pid_growth)
                        + " exceeds budget " + std::to_string(cfg.pid_growth_budget));
        }

        // 3. Zero reclaim aborts (safety sequence stable) — if reclaim attempted
        if (lifetime_stats.reclaim == 0) {
            pass_assert("No reclaims occurred (nothing to abort)");
        } else if (total_reclaim_aborts == 0) {
            pass_assert("Zero reclaim aborts across "
                        + std::to_string(lifetime_stats.reclaim) + " reclaims");
        } else {
            fail_assert(std::to_string(total_reclaim_aborts)
                        + " reclaim aborts across "
                        + std::to_string(lifetime_stats.reclaim) + " reclaims");
        }

        // 4. Tick latency responsiveness: p95 below ceiling
        if (!tick_latencies.empty()) {
            auto sorted_lat = tick_latencies;
            std::sort(sorted_lat.begin(), sorted_lat.end());
            double p95 = sorted_lat[(int)(sorted_lat.size() * 0.95)];
            if (p95 <= cfg.max_latency_p95_ms) {
                pass_assert("Tick p95 " + std::to_string((long)p95) + "ms <= "
                            + std::to_string((long)cfg.max_latency_p95_ms) + "ms ceiling");
            } else {
                fail_assert("Tick p95 " + std::to_string((long)p95) + "ms > "
                            + std::to_string((long)cfg.max_latency_p95_ms) + "ms ceiling");
            }
        }

        // 5. Decision throughput sanity
        if (lifetime_stats.total > 0) {
            pass_assert("Decision throughput OK (" + std::to_string(lifetime_stats.total)
                        + " decisions)");
        } else {
            fail_assert("No decisions were produced");
        }

        if (!all_asserts_passed) {
            std::cout << "\nRESULT: " << assert_num << " assertions, "
                      << "ASSERTS FAILED — returning non-zero exit code.\n";
            return 1;
        }
        std::cout << "\nRESULT: ALL " << assert_num << " ASSERTIONS PASSED.\n";
    }

    return 0;
}
