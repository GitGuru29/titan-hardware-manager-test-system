#!/usr/bin/env bash
# =============================================================================
# Analyse the 15-hour run once it finishes.
#
# The question the run exists to answer: does RSS growth flatten once the
# latency vectors reach kLatencyWindow = 100000?
#
#   plateau tick  = 100000
#   plateau time  = 100000 * 200ms = 20000s = 5.56h
#
# A saturating term predicts RSS flattening at ~2400 KB. A genuine unbounded
# leak at the ~430 KB/h rate seen across earlier campaigns would reach
# ~6450 KB by 15h and fail the 4096 KB budget.
# =============================================================================
set -uo pipefail

OUT="${THM_15H_OUT:-/home/msfvenom/thm-stress-15h}"
CSV="$OUT/stress_15h.csv"
LOG="$OUT/run.log"
SUM="$OUT/ANALYSIS.md"
PLATEAU_TICK=100000

[ -f "$CSV" ] || { echo "no CSV at $CSV"; exit 1; }

{
echo "# THM v3 15-hour stress run — analysis"
echo
echo "- generated: $(date -Is)"
echo "- commit: $(git -C "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)" rev-parse --short HEAD 2>/dev/null || echo unknown)"
echo "- source: \`$CSV\`"
echo

awk -F, -v P="$PLATEAU_TICK" '
NR==1 { next }
{
  n++; t=$1; tick=$2; rss=$16; pid=$17; psi=$19
  if (n==1) { t0=t; rss0=rss; pid0=pid; rss_max=rss; rss_max_t=t }
  if (rss>rss_max) { rss_max=rss; rss_max_t=t }
  if (psi+0>0) { pzin++; if (psi+0>psimax) psimax=psi; psisum+=psi }
  if ($18+0>memmax) memmax=$18
  if (psi+0>0) psi_last_pos=t
  last_t=t; last_rss=rss; last_pid=pid; last_tick=tick
  # bucket RSS growth per 10000 ticks so the shape of the curve is visible
  bucket=int(tick/10000)
  if (!(bucket in seen)) { seen[bucket]=1; bt[bucket]=t; br[bucket]=rss-rss0; bp[bucket]=pid-pid0 }
}
END {
  printf "## Scale\n\n"
  printf "- samples: %d\n- elapsed: %d s (%.2f h)\n- ticks: %d\n", n, last_t, last_t/3600, last_tick
  printf "- plateau would be reached at tick %d = %.2f h; %s\n", P, P*0.2/3600, (last_tick>=P ? "this run PASSES it" : "run ended BEFORE the plateau — plateau still unobservable")
  printf "\n## RSS growth (the ISSUE-01 question)\n\n"
  printf "- baseline: %d KB\n- final: %d KB\n- growth: **%d KB**\n", rss0, last_rss, last_rss-rss0
  printf "- peak: %d KB at t=%ds\n", rss_max, rss_max_t
  printf "- saturation model predicts ~2400 KB; a real ~430 KB/h leak predicts ~6450 KB by 15h\n"
  printf "\nGrowth per 10000 ticks — look for this flattening:\n\n"
  printf "| tick bucket | t (s) | RSS growth (KB) | PID growth |\n|---|---|---|---|\n"
  for (b=0; b in bt; b++) printf "| %d-%d | %d | %d | %d |\n", b*10000, b*10000+9999, bt[b], br[b], bp[b]
  printf "\nIf the RSS column stops rising after tick %d, the leak claim is refuted.\n", P
  printf "\n## PID growth\n\n"
  printf "- baseline: %d — final: %d — growth: **%d**\n", pid0, last_pid, last_pid-pid0
  printf "- assertion budget: 45000\n"
  printf "\n## Pressure / ISSUE-07\n\n"
  printf "- peak memory used: %.1f%%\n", memmax
  printf "- PSI > 0 in %d/%d samples (%.1f%%), peak %.2f\n", pzin, n, 100*pzin/n, psimax
  printf "- last positive PSI sample: t=%ds\n", psi_last_pos
  printf "- (note: `--aging-phase` replaces one spawn in three with an idle process, which suppresses PSI; see fixes report)\n"
}' "$CSV"

echo
echo "## Assertions (from harness)"
echo
if grep -q "RESULT:" "$LOG" 2>/dev/null; then
  sed -n '/PASS\/FAIL ASSERTIONS/,/RESULT:/p' "$LOG"
else
  echo "_run still in progress — assertions not yet emitted_"
  if [ -f "$OUT/RUN_STATUS" ]; then
    started=$(grep '^started=' "$OUT/RUN_STATUS" | cut -d= -f2)
    echo
    echo "started: $started  elapsed: $(($(date +%s) - $(date -d "$started" +%s 2>/dev/null || echo 0)))s / 54000s"
  fi
fi
} > "$SUM"

echo "wrote $SUM"
