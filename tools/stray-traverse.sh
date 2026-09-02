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
FRAME="$GAME_DIR/stray-frame.txt"
frame_now() { field "$FRAME" frame; }

log "traverse: hold UP, alternate LEFT/RIGHT every ${SWAP}s, ${TOTAL}s total (label=$LABEL)"
# The measurement instrument is the probe's engine frame counter (GFrameCounter via
# UKismetSystemLibrary), written at 4 Hz only while stray-probe-bench exists. It is the
# same two static calls in every arm, so no arm is measured differently from the
# baseline. The probe's state queries are silenced for the window at the same time
# (stray-probe-quiet): user-reported, a synchronous object scan is a visible spike.
QUIET="$GAME_DIR/stray-probe-quiet"; BENCH="$GAME_DIR/stray-probe-bench"
rm -f "$FRAME"; touch "$QUIET" "$BENCH"; chown deck:deck "$QUIET" "$BENCH" 2>/dev/null
trap 'rm -f "$QUIET" "$BENCH"' EXIT
for _ in $(seq 1 12); do [ -n "$(frame_now)" ] && break; sleep 0.25; done
COUNTER=1; [ -n "$(frame_now)" ] && [ "$(frame_now)" != "-1" ] || { COUNTER=0; log "no engine frame counter (probe too old, or GetFrameCount unavailable): timing only"; }

# The key script runs in the background so this shell can sample the counter meanwhile.
python3 "$INJECT" traverse "/dev/input/$KBD" "$TOTAL" "$SWAP" >/dev/null 2>&1 &
INJ=$!
TW0=$(date +%s.%N)
samples=(); dts=(); f_prev=$(frame_now); t_prev=$TW0; f_start=$f_prev
while kill -0 "$INJ" 2>/dev/null; do
    sleep 0.25
    tick_checks "traverse" "$T0"
    if [ "$COUNTER" -eq 1 ]; then
        f_now=$(frame_now); t_now=$(date +%s.%N)
        if [ "$f_now" != "$f_prev" ]; then
            samples+=("$(awk -v a="$f_prev" -v b="$f_now" -v ta="$t_prev" -v tb="$t_now" 'BEGIN { d = tb - ta; if (d > 0) printf "%.1f", (b - a) / d; else print "0" }')")
            dts+=("$(field "$FRAME" dt)")
            f_prev=$f_now; t_prev=$t_now
        fi
    fi
done
wait "$INJ" 2>/dev/null
TW1=$(date +%s.%N); elapsed=$(awk -v a="$TW0" -v b="$TW1" 'BEGIN { printf "%.1f", b - a }')
f_end=$(frame_now)
# Lift the flags and wait for one full probe write, so whatever runs next (another
# reload's require_ingame) reads engine state, not the quiet stub.
rm -f "$QUIET" "$BENCH"; trap - EXIT
for _ in $(seq 1 8); do [ -n "$(probe ingame)" ] && break; sleep 0.5; done

if [ "$COUNTER" -eq 1 ]; then
    stats=$(printf '%s\n' "${samples[@]}" | sort -n | awk -v frames="$(( f_end - f_start ))" -v el="$elapsed" '
        { v[NR] = $1 } END {
            n = NR; if (n == 0) { print "0,0,0,0"; exit }
            med = (n % 2) ? v[(n + 1) / 2] : (v[n / 2] + v[n / 2 + 1]) / 2
            h = 0; for (i = 1; i <= n; i++) if (v[i] < 0.6 * med) h++
            printf "%.1f,%.1f,%d,%d", frames / el, v[1], h, n }')
    IFS=, read -r avg slowest hitches n <<< "$stats"
    worst_dt=$(printf '%s\n' "${dts[@]}" | sort -g | tail -n 1)
    worst_ms=$(awk -v d="${worst_dt:-0}" 'BEGIN { printf "%.1f", d * 1000 }')
    log "RESULT $LABEL: avg ${avg} fps over ${elapsed}s (${f_start}->${f_end}, $(( f_end - f_start )) frames), slowest 0.25s bucket ${slowest} fps, hitch buckets ${hitches}/${n}, worst sampled frame ${worst_ms} ms"
else
    avg=""; slowest=""; hitches=""; n=0; worst_ms=""
    log "RESULT $LABEL: window completed in ${elapsed}s (no fps)"
fi
[ -f "$CSV" ] || echo "time,label,avg_fps,slowest_bucket_fps,hitch_buckets,buckets,worst_sampled_ms,elapsed_s,map" > "$CSV"
echo "$(date +%H:%M:%S),$LABEL,$avg,$slowest,$hitches,$n,$worst_ms,$elapsed,$(probe map)" >> "$CSV"
chown deck:deck "$CSV" 2>/dev/null
[ -n "$SHOT" ] && bash "$TOOLS_DIR/screenshot-gamescope.sh" "$SHOT" 4 >/dev/null 2>&1 && log "  screenshot: $SHOT"
exit 0
