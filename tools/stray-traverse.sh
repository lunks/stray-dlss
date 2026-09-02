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

TOTAL=15; SWAP=3; LABEL="run"; CSV="$GAME_DIR/stray-bench.csv"; SHOT=""; REPLAY=""
while [ $# -gt 0 ]; do
    case "$1" in
        --replay) REPLAY="$2"; shift 2 ;;    # a stray-record.sh file: the user's own input, replayed
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

if [ -n "$REPLAY" ]; then
    [ -s "$REPLAY" ] || { log "FAILED: recording $REPLAY is missing or empty"; exit 1; }
    log "traverse: replaying the recorded input $REPLAY ($(wc -l < "$REPLAY") events, $(tail -n 1 "$REPLAY" | cut -d' ' -f1)s) (label=$LABEL)"
else
    log "traverse: hold UP, alternate LEFT/RIGHT every ${SWAP}s, ${TOTAL}s total (label=$LABEL)"
fi
# The measurement instrument is the probe's engine frame counter (GFrameCounter via
# UKismetSystemLibrary), written at 4 Hz only while stray-probe-bench exists. It is the
# same two static calls in every arm, so no arm is measured differently from the
# baseline. The probe's state queries are silenced for the window at the same time
# (stray-probe-quiet): user-reported, a synchronous object scan is a visible spike.
QUIET="$GAME_DIR/stray-probe-quiet"; BENCH="$GAME_DIR/stray-probe-bench"
rm -f "$FRAME"; touch "$QUIET" "$BENCH"; chown deck:deck "$QUIET" "$BENCH" 2>/dev/null
trap 'rm -f "$QUIET" "$BENCH"' EXIT
# The probe's I/O and its engine reads alternate on different threads, so the FIRST
# write of a window carries whatever was collected last time: -1 on a fresh launch, or
# the previous window's final frame (measured 2026-09-02: that stale start inflated a
# window to 277 fps against a true ~140). Wait for a real value, then for it to CHANGE,
# and only then take the start frame.
for _ in $(seq 1 16); do v=$(frame_now); [ -n "$v" ] && [ "$v" != "-1" ] && break; sleep 0.25; done
COUNTER=1; v=$(frame_now); [ -n "$v" ] && [ "$v" != "-1" ] || { COUNTER=0; log "no engine frame counter (probe too old, or GetFrameCount unavailable): timing only"; }
if [ "$COUNTER" -eq 1 ]; then
    for _ in $(seq 1 8); do sleep 0.25; [ "$(frame_now)" != "$v" ] && break; done
    [ "$(frame_now)" != "$v" ] || { COUNTER=0; log "frame counter is not advancing: timing only"; }
fi

# The key script runs in the background so this shell can sample the counter meanwhile.
if [ -n "$REPLAY" ]; then
    python3 "$INJECT" replay "/dev/input/$KBD" "$REPLAY" >/dev/null 2>&1 &
else
    python3 "$INJECT" traverse "/dev/input/$KBD" "$TOTAL" "$SWAP" >/dev/null 2>&1 &
fi
INJ=$!
# The drive ratio: engine-TAA dispatches we suppressed (i.e. evaluated DLSS for) per present,
# read from the plugin's own status file over the window. ~1.0 means every frame was driven; a
# ratio below ~0.995 is a refusal streak and fails the pass whatever the fps (facts §29/§30).
drivefield() { grep "^$1=" "$GAME_DIR/stray-dlss-status.txt" 2>/dev/null | tail -1 | cut -d= -f2; }
ds0=$(drivefield native_drive_suppressed); pf0=$(drivefield frame)
TW0=$(date +%s.%N)
samples=(); dts=(); f_prev=$(frame_now); t_prev=$TW0; f_start=$f_prev; t_first=$TW0; t_last=$TW0; f_end=$f_prev
while kill -0 "$INJ" 2>/dev/null; do
    sleep 0.25
    tick_checks "traverse" "$T0"
    if [ "$COUNTER" -eq 1 ]; then
        f_now=$(frame_now); t_now=$(date +%s.%N)
        # A sample must be a number and monotonic; anything else is a torn read of the
        # probe file (it produced a "-27749.5 fps" bucket before writes were made atomic).
        case "$f_now" in ''|*[!0-9]*) continue ;; esac
        [ "$f_now" -lt "$f_prev" ] && continue
        if [ "$f_now" != "$f_prev" ]; then
            samples+=("$(awk -v a="$f_prev" -v b="$f_now" -v ta="$t_prev" -v tb="$t_now" 'BEGIN { d = tb - ta; if (d > 0) printf "%.1f", (b - a) / d; else print "0" }')")
            dts+=("$(field "$FRAME" dt)")
            f_prev=$f_now; t_prev=$t_now; f_end=$f_now; t_last=$t_now
        fi
    fi
done
wait "$INJ" 2>/dev/null
# Average over the SAMPLED span (first to last counter change), so the file's <=250 ms
# lag at either end cancels instead of biasing the window.
elapsed=$(awk -v a="$t_first" -v b="$t_last" 'BEGIN { printf "%.1f", b - a }')
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
ds1=$(drivefield native_drive_suppressed); pf1=$(drivefield frame)
drive_ratio=$(awk -v a="${ds0:-}" -v b="${ds1:-}" -v pa="${pf0:-}" -v pb="${pf1:-}" 'BEGIN { if (a=="" || b=="" || pa=="" || pb=="") { print ""; exit } d = pb - pa; if (d > 0) printf "%.3f", (b - a) / d; else print "" }')
[ -n "$drive_ratio" ] && log "DRIVE RATIO $LABEL: $drive_ratio (suppressed $ds0->$ds1 over presents $pf0->$pf1) - <0.995 is a refusal streak"
[ -f "$CSV" ] || echo "time,label,avg_fps,slowest_bucket_fps,hitch_buckets,buckets,worst_sampled_ms,elapsed_s,drive_ratio,map" > "$CSV"
echo "$(date +%H:%M:%S),$LABEL,$avg,$slowest,$hitches,$n,$worst_ms,$elapsed,${drive_ratio:-},$(probe map)" >> "$CSV"
chown deck:deck "$CSV" 2>/dev/null
[ -n "$SHOT" ] && bash "$TOOLS_DIR/screenshot-gamescope.sh" "$SHOT" 4 >/dev/null 2>&1 && log "  screenshot: $SHOT"
exit 0
