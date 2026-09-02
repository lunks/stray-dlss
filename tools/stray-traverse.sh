#!/usr/bin/env bash
# tools/stray-traverse.sh [--total S] [--swap S] [--label TEXT] [--csv PATH] [--shot PATH]
# The user's perf scenario (2026-09-02): hold ARROW UP for the whole run and, while it is
# held, alternate ARROW LEFT and ARROW RIGHT every 3 s, for 15 s total. From the same
# checkpoint the cat walks a deterministic path through areas where a speed difference is
# obvious, and the alternation reproduces the frame-pacing hitches the user feels.
#
# Measurement: the render host heartbeat's frame counter (rewritten every 30 frames) is
# sampled every 0.5 s during the window. Reported: avg fps over the window, the slowest
# 0.5 s sample, and hitches = samples below 60% of the median sample. Coarse by design —
# the host's own [perf] histogram gives p99; this gives a host-independent, window-exact
# number. With no render host active only the elapsed time is reported.
# Appends one CSV row (stray-bench.csv in the game dir by default). Exit 0 = window
# completed, 3 = the game crashed inside it. Composable with stray-reload.sh.
set -u
. "$(dirname "$0")/stray-lib.sh"

TOTAL=15; SWAP=3; LABEL="run"; CSV="$GAME_DIR/stray-bench.csv"; SHOT=""
while [ $# -gt 0 ]; do
    case "$1" in
        --total) TOTAL="$2"; shift 2 ;;
        --swap) SWAP="$2"; shift 2 ;;
        --label) LABEL="$2"; shift 2 ;;
        --csv) CSV="$2"; shift 2 ;;
        --shot) SHOT="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

require_ingame
T0=$(date +%s)
HB=0; [ -f "$STATUS" ] && [ "$(( T0 - $(mtime "$STATUS") ))" -le 5 ] && HB=1
[ "$HB" -eq 1 ] || log "no render-host heartbeat: timing only, no fps"

log "traverse: hold UP, alternate LEFT/RIGHT every ${SWAP}s, ${TOTAL}s total (label=$LABEL)"
# The key script runs in the background so this shell can sample the heartbeat meanwhile.
python3 "$INJECT" traverse "/dev/input/$KBD" "$TOTAL" "$SWAP" >/dev/null 2>&1 &
INJ=$!

samples=(); f_prev=$(hb_frame); t_prev=$(date +%s.%N); f_start=$f_prev
while kill -0 "$INJ" 2>/dev/null; do
    sleep 0.5
    tick_checks "traverse" "$T0"
    if [ "$HB" -eq 1 ]; then
        f_now=$(hb_frame); t_now=$(date +%s.%N)
        fps=$(awk -v a="$f_prev" -v b="$f_now" -v ta="$t_prev" -v tb="$t_now" 'BEGIN { d = tb - ta; if (d > 0) printf "%.1f", (b - a) / d; else print "0" }')
        samples+=("$fps"); f_prev=$f_now; t_prev=$t_now
    fi
done
wait "$INJ" 2>/dev/null
T1=$(date +%s.%N); elapsed=$(awk -v a="$T0" -v b="$T1" 'BEGIN { printf "%.1f", b - a }')

if [ "$HB" -eq 1 ]; then
    f_end=$(hb_frame)
    stats=$(printf '%s\n' "${samples[@]}" | sort -n | awk -v frames="$(( f_end - f_start ))" -v el="$elapsed" '
        { v[NR] = $1 } END {
            n = NR; if (n == 0) { print "0,0,0,0"; exit }
            med = (n % 2) ? v[(n + 1) / 2] : (v[n / 2] + v[n / 2 + 1]) / 2
            h = 0; for (i = 1; i <= n; i++) if (v[i] < 0.6 * med) h++
            printf "%.1f,%.1f,%d,%d", frames / el, v[1], h, n }')
    IFS=, read -r avg slowest hitches n <<< "$stats"
    log "RESULT $LABEL: avg ${avg} fps over ${elapsed}s, slowest 0.5s sample ${slowest} fps, hitches ${hitches}/${n} (frames $(( f_end - f_start )))"
else
    avg=""; slowest=""; hitches=""; n=0
    log "RESULT $LABEL: window completed in ${elapsed}s (no fps: no heartbeat)"
fi
[ -f "$CSV" ] || echo "time,label,avg_fps,slowest_fps,hitches,samples,elapsed_s,map" > "$CSV"
echo "$(date +%H:%M:%S),$LABEL,$avg,$slowest,$hitches,$n,$elapsed,$(probe map)" >> "$CSV"
chown deck:deck "$CSV" 2>/dev/null
[ -n "$SHOT" ] && bash "$TOOLS_DIR/screenshot-gamescope.sh" "$SHOT" 4 >/dev/null 2>&1 && log "  screenshot: $SHOT"
exit 0
