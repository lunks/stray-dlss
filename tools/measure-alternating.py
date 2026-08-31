#!/usr/bin/env python3
"""Measure whether suppressing one pass changes the image, honestly.

Runs INSIDE CT113. Captures N frames while the add-on alternates a named pass between
suppressed and normal, classifies each frame by which phase it landed in, and reports
high-frequency energy per group.

Why it is built this way — three flaws defeated earlier attempts and all three had to go:
  * a stationary camera hides temporal effects, because TAA's history is already converged,
    so every capture pans the right stick first;
  * a moving camera makes separate runs incomparable, so both states are sampled inside ONE
    session via the add-on's DryRunAlternate and its "ALT PHASE" log lines;
  * mean RGB and cyan counts cannot see the difference, so the metric is high-frequency
    energy, which aliasing raises and temporal accumulation lowers.

Prints a table and the group statistics. It draws no conclusion: with a moving camera the
scene varies, so the spread matters as much as the means.
"""
import glob, os, re, struct, subprocess, sys, time

GAME_DIR = "/run/media/deck/GamesLinux/SteamLibrary/steamapps/common/Stray/Hk_project/Binaries/Win64"
LOG = os.path.join(GAME_DIR, "ReShade.log")
KEY_SYSRQ, EV_KEY, EV_SYN, EV_ABS, ABS_RX = 99, 0x01, 0x00, 0x03, 0x03


def node(match_name=None, need_sysrq=False):
    name = None
    for line in open("/proc/bus/input/devices"):
        line = line.strip()
        if line.startswith("N: Name="):
            name = line.split("=", 1)[1].strip('"')
        elif line.startswith("H: Handlers="):
            if need_sysrq and "sysrq" not in line:
                continue
            if match_name and name != match_name:
                continue
            for tok in line.split("=", 1)[1].split():
                if tok.startswith("event"):
                    return "/dev/input/" + tok
    return None


def emit(f, typ, code, val):
    f.write(struct.pack("llHHi", 0, 0, typ, code, val))
    f.flush()


def pan(pad, seconds=1.0):
    if not pad:
        return
    with open(pad, "wb") as f:
        end = time.time() + seconds
        while time.time() < end:
            emit(f, EV_ABS, ABS_RX, 22000)
            emit(f, EV_SYN, 0, 0)
            time.sleep(0.05)
        emit(f, EV_ABS, ABS_RX, 0)
        emit(f, EV_SYN, 0, 0)


def shoot(kb, before):
    with open(kb, "wb") as f:
        emit(f, EV_KEY, KEY_SYSRQ, 1); emit(f, EV_SYN, 0, 0)
        time.sleep(0.05)
        emit(f, EV_KEY, KEY_SYSRQ, 0); emit(f, EV_SYN, 0, 0)
    for _ in range(30):
        time.sleep(0.4)
        new = set(glob.glob(os.path.join(GAME_DIR, "*.png"))) - before
        if new:
            return max(new, key=os.path.getmtime)
    return None


def hf_energy(path):
    """Laplacian standard deviation: aliasing raises it, temporal accumulation lowers it."""
    out = subprocess.run(["magick", path, "-colorspace", "gray", "-morphology", "Convolve",
                          "Laplacian:0", "-format", "%[fx:standard_deviation*100000]", "info:"],
                         capture_output=True, text=True)
    try:
        return float(out.stdout.strip())
    except ValueError:
        return None


def phase_intervals():
    """(seconds_of_day, suppressing) transitions from the add-on's own log."""
    out = []
    pat = re.compile(r"^(\d\d):(\d\d):(\d\d):\d+ .*ALT PHASE (SUPPRESSING|normal)")
    for line in open(LOG, "rb").read().decode("utf-8", "replace").splitlines():
        m = pat.match(line.strip())
        if m:
            h, mi, s = int(m.group(1)), int(m.group(2)), int(m.group(3))
            out.append((h * 3600 + mi * 60 + s, m.group(4) == "SUPPRESSING"))
    return out


def classify(mtime, transitions):
    lt = time.localtime(mtime)
    secs = lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec
    state = None
    for t, sup in transitions:
        if t <= secs:
            state = sup
        else:
            break
    return state


def main():
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 40
    kb = node(need_sysrq=True)
    pad = node("Microsoft X-Box 360 pad 0")
    if not kb:
        raise SystemExit("no sysrq keyboard node")
    print(f"keyboard={kb} pad={pad} samples={count}", flush=True)

    rows = []
    for i in range(count):
        before = set(glob.glob(os.path.join(GAME_DIR, "*.png")))
        pan(pad)
        shot = shoot(kb, before)
        if not shot:
            print(f"{i}: no shot", flush=True)
            continue
        e = hf_energy(shot)
        rows.append((os.path.getmtime(shot), e, shot))
        print(f"{i}: hf={e:.0f}", flush=True)
        os.remove(shot)          # 5 MB each; the number is what matters

    tr = phase_intervals()
    groups = {True: [], False: []}
    for mt, e, _ in rows:
        st = classify(mt, tr)
        if st is not None and e is not None:
            groups[st].append(e)

    print("\n=== RESULT ===")
    for st, label in ((True, "SUPPRESSED"), (False, "normal    ")):
        v = groups[st]
        if not v:
            print(f"{label}: no samples")
            continue
        v.sort()
        mean = sum(v) / len(v)
        var = sum((x - mean) ** 2 for x in v) / len(v)
        print(f"{label}: n={len(v):3d} mean={mean:9.0f} sd={var ** 0.5:8.0f} "
              f"median={v[len(v) // 2]:9.0f} min={v[0]:9.0f} max={v[-1]:9.0f}")


if __name__ == "__main__":
    main()
