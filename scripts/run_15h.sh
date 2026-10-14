#!/usr/bin/env bash
# =============================================================================
# 15-Hour THM v3 stress run — single continuous process
#
# Why single process (not 3 x 5h, which is how the 10h run was staged):
#   The stress harness keeps tick/pipeline latency in two std::vector<double>
#   ring buffers sized kLatencyWindow = 100000. At 200ms ticks that cap is
#   reached at 100000 ticks = 5.56h. The 10h run's two 5h stages each reset
#   the count to zero, so neither could ever reach it — which is exactly why
#   ISSUE-01 ("unbounded RSS leak") was never falsified. This run reaches
#   270000 ticks, 2.7x the cap, so the plateau is observable.
#
# ISSUE-07 is fixed and active here: --memory-floor-mb 2048 ramps real
# anonymous memory down until MemAvailable <= 2048 MB and holds it. Probed
# at 65% psi>0 samples, peak PSI 8.1, and compute_pressure() reaching
# CRITICAL on its own — so the forced CRITICAL below agrees with reality
# instead of the run being synthetic.
#
# ISSUE-06 caveat still applies: this runs as uid 1000, so cgroup writes
# (cpu.weight / cgroup.freeze / memory.high) fail. The enforcement plane is
# measured as decisions, not as applied control.
# =============================================================================
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${THM_15H_OUT:-/home/msfvenom/thm-stress-15h}"
HARNESS="$REPO/build/thm_stress_test"

DURATION="${THM_15H_DURATION:-54000}"   # 15h
TICK_MS=200
FORCE=CRITICAL                           # opt 1 — chosen over real-pressure staging
FLOOR_MB=2048                            # ISSUE-07
RSS_BUDGET_KB=4096                       # plateau model predicts ~2400 KB growth
RSS_BUDGET_PCT=200                       # deliberately non-binding; KB budget is the gate
PID_BUDGET=45000                         # 15h at the ~1800/h rate observed = ~27000
P95_CEILING=1500

mkdir -p "$OUT"
cd "$REPO"

CSV="$OUT/stress_15h.csv"
LOG="$OUT/run.log"

cat > "$OUT/RUN_STATUS" <<EOF
status=running
started=$(date -Is)
duration_sec=$DURATION
tick_ms=$TICK_MS
force_pressure=$FORCE
aging_phase=1
memory_floor_mb=$FLOOR_MB
rss_budget_kb=$RSS_BUDGET_KB
rss_budget_pct=$RSS_BUDGET_PCT
pid_budget=$PID_BUDGET
p95_ceiling_ms=$P95_CEILING
csv=$CSV
commit=$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo unknown)
user=$(id -un) uid=$(id -u)
command=thm_stress_test --duration $DURATION --tick-ms $TICK_MS \\
    --force-pressure $FORCE --aging-phase --memory-floor-mb $FLOOR_MB \\
    --assert $P95_CEILING --pid-budget $PID_BUDGET \\
    --rss-budget-pct $RSS_BUDGET_PCT --rss-budget-kb $RSS_BUDGET_KB
EOF

# systemd-inhibit blocks lid/battery/idle suspend for exactly as long as the
# harness runs; the inhibitor exits when it does, so nothing outlives the run.
exec systemd-inhibit \
    --what=sleep:idle \
    --who=thm-stress \
    --why="15-hour THM v3 stress run" \
    --mode=block \
    "$HARNESS" \
    --duration "$DURATION" \
    --tick-ms "$TICK_MS" \
    --csv "$CSV" \
    --force-pressure "$FORCE" \
    --aging-phase \
    --memory-floor-mb "$FLOOR_MB" \
    --assert "$P95_CEILING" \
    --pid-budget "$PID_BUDGET" \
    --rss-budget-pct "$RSS_BUDGET_PCT" \
    --rss-budget-kb "$RSS_BUDGET_KB" \
    > "$LOG" 2>&1
