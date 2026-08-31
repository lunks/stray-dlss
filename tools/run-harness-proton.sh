#!/usr/bin/env bash
# Run the D3D12 harness against the REAL GPU, under the same Proton/vkd3d-proton build
# Stray uses — without launching Stray, and without disturbing a live game session.
#
# Why this exists: CI runs the same binary against WARP. WARP models D3D12's rules
# faithfully but is not the layer we ship on, and vkd3d-proton in a release Proton
# validates root parameter indices and types by assert() only — so it fails silently.
# This closes that gap. CI stays the primary gate; this is the supplementary one.
#
# ISOLATION — both of these are load-bearing, learned the hard way:
#
#   * PRIVATE DISPLAY. Never reuse the game's DISPLAY. It is gamescope's nested X server;
#     Wine creates windows as soon as winex11.drv initialises, gamescope focuses the newest
#     toplevel, and a harness that never presents a frame freezes the compositor on the
#     game's last frame. That looks exactly like a game hang and is not one. `xvfb-run -a`
#     picks a display nobody is using.
#
#   * PRIVATE WINE PREFIX. Never reuse compatdata/1332010. All Wine processes in a prefix
#     share one wineserver, so running here would join the live game's. Proton's `run` verb
#     also performs prefix setup that assumes exclusive access.
set -euo pipefail

HOST="${HOST:-root@192.168.0.210}"
CT="${CT:-113}"                                    # SteamOS lives in LXC 113 on the Proxmox host
PROTON="/home/deck/.local/share/Steam/compatibilitytools.d/GE-Proton-dxvk301-ds5-clean-nowl/proton"
REMOTE_DIR="/tmp/stray-dlss-harness"
PRIVATE_COMPAT="$REMOTE_DIR/compat"                # our own prefix, NOT the game's
EXE="${1:-}"

# WITH_RESHADE=1 also stages ReShade as d3d12.dll and our add-on beside the harness, so the
# run goes through ReShade's real proxy against real vkd3d-proton — the only place the
# ID3D12DeviceExt vtable hook can actually be observed, since it needs vkd3d's extension
# interface to exist at all.
RESHADE_DLL="${RESHADE_DLL:-}"
ADDON="${ADDON:-}"

if [[ -z "$EXE" || ! -f "$EXE" ]]; then
	echo "usage: $0 <path-to-stray-dlss-harness.exe>" >&2
	exit 2
fi

if ssh -o ConnectTimeout=8 "$HOST" "pgrep -f Stray-Win64-Shipping.exe >/dev/null"; then
	echo "NOTE: Stray is running. Using a private display and prefix, so it is not touched."
fi

echo "==> copying harness into CT $CT"
ssh -o ConnectTimeout=8 "$HOST" "mkdir -p /tmp/stray-dlss-stage"
scp -q "$EXE" "$HOST:/tmp/stray-dlss-stage/harness.exe"
ssh "$HOST" "pct exec $CT -- mkdir -p $REMOTE_DIR $PRIVATE_COMPAT && \
	pct push $CT /tmp/stray-dlss-stage/harness.exe $REMOTE_DIR/harness.exe && \
	pct exec $CT -- chown -R deck:deck $REMOTE_DIR && \
	pct exec $CT -- chmod 755 $REMOTE_DIR/harness.exe"

if [[ "${WITH_RESHADE:-0}" == "1" ]]; then
	if [[ ! -f "$RESHADE_DLL" || ! -f "$ADDON" ]]; then
		echo "WITH_RESHADE=1 needs RESHADE_DLL=<ReShade64.dll> and ADDON=<*.addon64>" >&2
		exit 2
	fi
	echo "==> staging ReShade and the add-on"
	scp -q "$RESHADE_DLL" "$HOST:/tmp/stray-dlss-stage/d3d12.dll"
	scp -q "$ADDON" "$HOST:/tmp/stray-dlss-stage/stray-dlss.addon64"
	ssh "$HOST" "pct push $CT /tmp/stray-dlss-stage/d3d12.dll $REMOTE_DIR/d3d12.dll && \
		pct push $CT /tmp/stray-dlss-stage/stray-dlss.addon64 $REMOTE_DIR/stray-dlss.addon64 && \
		pct exec $CT -- sh -c 'printf \"[GENERAL]\\nAddonPath=.\\n[ADDON]\\nAddonPath=.\\n\" > $REMOTE_DIR/ReShade.ini' && \
		pct exec $CT -- chown -R deck:deck $REMOTE_DIR"
	# n,b = prefer the native DLL sitting next to the exe over Wine's builtin.
	RESHADE_ENV="WINEDLLOVERRIDES=d3d12=n,b"
	HARNESS_ARGS="--hardware --expect-reshade"
else
	RESHADE_ENV=""
	HARNESS_ARGS="--hardware"
fi

echo "==> running against the real adapter on a private Xvfb display"
# Wine gives a console app no console here, so its stdout is lost. Redirect inside Windows
# instead, via cmd, and read the file back. Z: maps to the container's filesystem root.
VALIDATION_ENV=""
[[ "${VALIDATE:-0}" == "1" ]] && VALIDATION_ENV='VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation'

ssh "$HOST" "pct exec $CT -- setpriv --reuid=1001 --regid=1001 --clear-groups \
	xvfb-run -a --server-args='-screen 0 640x480x24' \
	env HOME=/home/deck USER=deck XDG_RUNTIME_DIR=/run/user/1001 \
		STEAM_COMPAT_CLIENT_INSTALL_PATH=/home/deck/.local/share/Steam \
		STEAM_COMPAT_DATA_PATH=$PRIVATE_COMPAT \
		VKD3D_DEBUG=${VKD3D_DEBUG:-none} DXVK_LOG_LEVEL=none WINEDEBUG=-all \
		$VALIDATION_ENV $RESHADE_ENV \
		$PROTON run 'C:\\windows\\system32\\cmd.exe' /c \
		'Z:\\tmp\\stray-dlss-harness\\harness.exe --hardware > Z:\\tmp\\stray-dlss-harness\\out.txt 2>&1' \
	>/dev/null 2>&1 || true"

echo
ssh "$HOST" "pct exec $CT -- cat $REMOTE_DIR/out.txt 2>/dev/null" | tee /tmp/harness-hw.log
if [[ "${WITH_RESHADE:-0}" == "1" ]]; then
	echo
	echo "--- add-on log ---"
	ssh "$HOST" "pct exec $CT -- cat $REMOTE_DIR/stray-dlss.log 2>/dev/null" | tail -20
fi
echo
if ! grep -q "^PASS" /tmp/harness-hw.log; then
	echo "HARNESS DID NOT PASS on hardware" >&2
	exit 1
fi
