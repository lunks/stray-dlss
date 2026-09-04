#!/usr/bin/env bash
# tools/stray-box.sh - the ONE way anything touches the test box.
#
# The box is a single shared resource: one GPU, one game process, one pad, one Steam.
# Two agents driving it at once is not a slow test, it is a WRONG one - a launch racing a
# deploy reads the previous session's log, and two launchers fight over the reaper. So every
# use takes an exclusive lock first, and a second user is told who holds it rather than
# quietly interleaving.
#
#   tools/stray-box.sh --who "<name>" [--wait S] -- <shell run inside container 113>
#   tools/stray-box.sh --status            # who holds it, and since when
#   tools/stray-box.sh --who X --wait 0 -- ...   # fail at once if busy (exit 75)
#
# The lock is an flock held by the remote shell, so a dropped ssh releases it. It lives on
# tmpfs, so a reboot clears it. Neither can wedge the box the way the old launcher did.
set -u

BOX_HOST="${STRAY_BOX_HOST:-root@192.168.0.210}"
BOX_CT="${STRAY_BOX_CT:-113}"
LOCK=/run/lock/stray-box.lock
OWNER=/run/lock/stray-box.owner

WHO=""; WAIT=1800; MODE=run
while [ $# -gt 0 ]; do
    case "$1" in
        --who)    WHO="$2"; shift 2 ;;
        --wait)   WAIT="$2"; shift 2 ;;
        --status) MODE=status; shift ;;
        --)       shift; break ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

ssh_box() { ssh -o ConnectTimeout=10 -o ServerAliveInterval=15 -o ServerAliveCountMax=6 \
                -o BatchMode=yes "$BOX_HOST" "$@"; }

if [ "$MODE" = status ]; then
    # ASK THE LOCK, NOT THE OWNER FILE. The file is written under the lock and removed by an
    # EXIT trap, so a shell that is SIGKILLed leaves it behind and the box reads BUSY forever
    # while it is actually free (observed 2026-09-04). flock -n is the authoritative test; the
    # owner file is only ever a label on top of it.
    ssh_box "pct exec $BOX_CT -- bash -c '
        if flock -n 9 2>/dev/null 9>$LOCK; then
            if [ -s $OWNER ]; then
                echo \"FREE (stale owner file from a killed shell: \$(cat $OWNER))\"
                rm -f $OWNER
            else
                echo FREE
            fi
        else
            echo \"BUSY: \$(cat $OWNER 2>/dev/null || echo \"holder did not name itself\")\"
        fi'"
    exit $?
fi

[ -n "$WHO" ] || { echo "--who is required: name yourself so the next agent knows who to wait for" >&2; exit 2; }
[ $# -gt 0 ] || { echo "nothing to run: pass the command after --" >&2; exit 2; }

# base64 the payload so the caller's quoting survives ssh -> pct exec -> bash -c intact.
CMD_B64=$(printf '%s' "$*" | base64 | tr -d '\n')

ssh_box "pct exec $BOX_CT -- bash -c '
    exec 9>$LOCK
    if ! flock -w $WAIT 9; then
        echo \"BOX BUSY after ${WAIT}s: \$(cat $OWNER 2>/dev/null || echo unknown)\" >&2
        exit 75
    fi
    printf \"%s pid=%s since=%s\\n\" \"$WHO\" \"\$\$\" \"\$(date -Is)\" > $OWNER
    trap \"rm -f $OWNER\" EXIT
    echo \"$CMD_B64\" | base64 -d | bash
'"
