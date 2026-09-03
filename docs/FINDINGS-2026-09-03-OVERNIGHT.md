# Overnight test cycle, 2026-09-03 04:20-05:05 — three verdicts, all measured

Run unattended on the user's box with `launch-stray-safe.sh`, the traverse load, and the
plugin's own `[fg]` line as the pacing instrument. Every number here came off that machine.

## 1. The DLSS-G flip-queue throttle does NOT work here. NEGATIVE, reverted.

Branch `fg-flip-throttle` @ `a63faa0`. Both arms ran the same binary with only the ini
differing, same traverse load, compared at frame 6000:

| | throttle OFF | throttle ON |
|---|---|---|
| pacer median | 20.01 ms | 20.34 ms |
| hitches | 4 | 4 |
| holds / catchups | 2547 / 743 | 2594 / 757 |
| reanchors | 9 | 4 |
| issued-interval | p50=9 p99=14 unimodal | p50=9 p99=14 unimodal |
| blocked mean | — | **0.02 ms** |

**It armed perfectly and did nothing**: `ARMED(none) flag=added latency 2->2 waits=11814
slots=11814 timeouts=0 failed=0 bypassed=0`. Every wait got a slot immediately. `bypassed=0`
is what makes this a real negative rather than the counting-semaphore surplus the author
warned about — the flip queue never fills, so there was nothing to bound.

**Decisive:** at frame 1200, where the BIMODAL reading actually lives, the throttled arm is
**still BIMODAL** (`p50=8 p99=16`, blocked mean 0.08 ms), indistinguishable from the recorded
baseline (`p50=8 p99=15 BIMODAL`, holds 325/145/5).

**So the premise was wrong.** "Back-to-back presents" is not queue pressure — it is our own
present-twice pairing, which is what the design does by construction. vkd3d drains the queue
fine. Only changing how the pair is SPACED could move that number, and it resolves to
unimodal by frame 1800 anyway, so it is a warmup transient.

What the run did prove: **DXVK/vkd3d accept a waitable swapchain under gamescope** and the
game boots and runs with it. That was the one risk that could have stopped startup.

## 2. The FG camera pair: no measurable difference. Restored on principle, not on data.

Per 1000 frames, same load:

| | 75000/0.01 | 0/0 (old) |
|---|---|---|
| hitches | 0.6 | 0.8 |
| holds | 374 | 384 |
| catchups | 111 | 137 |
| pacer median | 19.61 ms | 21.31 ms |
| **bad crops** | **0** | **0** |

Both arms produced zero black, stale, suspect or dark crops, so `CameraFar=0` is not causing
degenerate interpolation that the validator can see. The pacing gaps sit inside the
run-to-run noise measured all night (19.3-21.3 ms across identical configs).

**Left at 75000/0.01** because it is what NVIDIA ships on both engine generations and is
HARD-justified, NOT because the data prefers it. Any real effect is on how interpolated
frames LOOK, which this instrument cannot see and which needs a human.

## 3. The pad audio route: Sony ACCEPTED it. Speaker audibility still unverified.

```
pad audio: scePadIsSupportedAudioFunction=0x00000001
           scePadSetAudioOutPath(3=SPEAKER)=0x00000000
           scePadSetVolumeGain(spk=80)=0x00000000   [handle=0x101, pad adopted]
pad audio: SONY ACCEPTED the route.
```

No proxy DLL, no hand-written HID bytes, handle straight from `scePadGetHandle` against the
already-loaded `libScePad.dll`. The purr then fires both `StartPS5Vibration 'CatPurr2_VIBE'`
and `StartPS5ControllerSound 'cat_purr_loop_01_CONTROL'`, submix handover at peak 0.231,
coils unchanged. **Whether the pad's speaker is audible only the user can say.**

## 4. The old launcher lies. Use `launch-stray-safe.sh`.

`tools/launch-stray.sh` reported `IN GAME (census=10, taa_pipelines=1)` while the in-engine
probe read `pawn=0 pc=1 map=HK_Project_MainStart ingame=0` — the menu. Two test cycles were
wasted on it, and pressing Enter blindly to compensate drove the game into `CreditsMap`.
`launch-stray-safe.sh` gates on the probe (`pawn=1 map=BaseMap ingame=1`) and reached
gameplay first try, every time after.

## 5. The purr fixture, and a wrong turn worth recording

`tools/data/stray-recording-purr.txt` — 38 events, hold Up 0.69 s then ONE tap of Q.
Reproduces the purr from a checkpoint. Two things learned the hard way:

* **One press of Q, not several.** Q is contextual and repeated presses cancel it; three taps
  produced nothing all evening.
* **The walk POSITIONS the cat — do not trim it out.** A bare Q appeared to work once, so the
  fixture was "fixed" down to the Q alone; it then failed twice. The Q had only worked
  because a walk replay had run moments earlier. The original is correct.
* **Allow ~20 s of settle after `ingame=1`** before replaying; a replay fired 6 s in does
  nothing.

## 6. A key can be left stranded, and it looks like the game is possessed

An ad-hoc injector killed between key-down and key-up latches that key at the kernel input
layer — the user saw `Q` "pressing indefinitely" with no process running. `inject.py replay`
releases held keys on exit, including on Ctrl-C. **Never write a bespoke injector**; use
`inject.py`.
