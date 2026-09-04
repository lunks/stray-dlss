# Using the test box — the rules, the lock, and the tooling

The box is **one** shared resource: one GPU, one game process, one pad, one Steam client,
one game directory. Two agents on it at once is not a slower test, it is a **wrong** one —
a launch racing a deploy reads the previous session's log, two launchers fight over the
reaper, and a bench under someone else's load is invalid by construction (§31).

**So every use takes the lock first.** No exceptions, including a "quick" log read that
kills the game to get a fresh one.

## The lock

```bash
tools/stray-box.sh --status                       # FREE, or BUSY: <who> pid=<n> since=<t>
tools/stray-box.sh --who "<your name>" -- '<shell run inside container 113>'
tools/stray-box.sh --who "<name>" --wait 0 -- ... # exit 75 at once if busy
```

`--wait` defaults to 1800 s. The lock is an `flock` held by the remote shell, so a dropped
ssh releases it and a stuck agent cannot wedge the box; it lives on tmpfs, so a reboot
clears it. Name yourself honestly in `--who` — it is the only thing the next agent sees.

## Hard rules

* **Never drive the game with injected input unless the user has said so for that session.**
  The default is `--no-drive` and reading the verdict from the MAIN MENU: the TAA pass runs
  there and the engine seam fires on frame 0, so the menu is a complete test for every
  DLSS-side hook. Benching is the exception and needs an explicit go-ahead each time.
* **Never restart Steam, never SIGKILL the launch tree.** Those recovery paths wedged the
  box four times in an hour. `launch-stray-safe.sh` is the only launcher.
* **`pgrep -f` self-matches the shell running it.** Use `pgrep -x Stray-Win64-Shi`, or the
  `13320[1]0` bracket trick for the reaper.
* **Kill the game before deploying**, and wait for the reaper to go.
* **Pass every `[STRAYDLSS]` key on every deploy** — the deploy writes only what it is given,
  so stale keys survive and silently change the run.
* **Check the log's timestamps are from the NEW session.** A grep matches yesterday just as
  happily.
* **A quiet host is a precondition, not a detail.** The container shares a Proxmox host;
  `stray-bench.sh` refuses a cycle above `load1/nproc = 0.5` for this reason.

## The tooling, and what each one is for

| Tool | Runs where | What it is for |
|---|---|---|
| `tools/stray-box.sh` | your machine | the lock; wraps everything below |
| `/tmp/launch-stray-safe.sh [--kill] [--no-drive] [--timeout S]` | box | the only launcher. Names every precondition, never restarts Steam, writes `stray-launch-verdict.txt` |
| `/tmp/stray-bench.sh --runs N --label L` | box | N x (reload + traverse) in ONE process. Needs a live gameplay session and injected input |
| `/tmp/stray-reload.sh` | box | checkpoint reload; accepted when the game unpauses itself |
| `/tmp/stray-traverse.sh` | box | the 15 s scenario, one CSV row in `stray-bench.csv` |
| `/tmp/screenshot-gamescope.sh` | box | `gamescopectl screenshot`, needs no ReShade |
| `mods/StrayProbe` | in-engine | `stray-game-state.txt` once a second: pawn, map, paused, ingame |
| `mods/StrayConsole` | in-engine | live console commands — sets cvars at `ECVF_SetByConsole`, above `[SystemSettings]`, so most tuning needs no relaunch and no ini edit |

Staging: `scp` to `root@192.168.0.210:/tmp/`, then `pct push 113 /tmp/<f> /tmp/<f> --perms 755`.
**`/tmp` on the box does not survive a reboot** — re-stage after one.

## Reading a verdict

`stray-dlss-plugin.log` in the game directory. The lines that decide things:

* `ENGINE SEAM FOUND` / `INSTALLED` — the vtable, validated by three constants. `candidates:
  name=1 getDebugName=1 vtable=1` means the scan was forced, not guessed.
* `[view] frame N` — `bad=0` on the row-135 self-check; `ambClaimed` is where DLSS SR had to
  guess between two legal Views.
* `unclaimed=` — frames where the engine announced a pass we did not claim. **This is the
  flicker metric**; 0 is the target.
* `[fg] frame N` — presents ratio (2.00x is steady state), and the crop gate's
  `black/stale/suspect` counters, which are what stop an unvalidated generated frame reaching
  the screen.
* `NR STAGE` — `applied` against `triggered`, and the refusal reasons by name.
