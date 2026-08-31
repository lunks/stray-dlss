#!/usr/bin/env bash
# Run the D3D12 harness on the SteamOS box against the REAL GPU, under the exact
# Proton/vkd3d-proton build Stray itself uses — without launching Stray.
#
# Why this exists: CI runs the same binary against WARP, which models D3D12's rules
# faithfully but is not the layer we ship on. Every bug that has cost us a session on
# the target box lived in the gap between "legal D3D12" and "what vkd3d-proton does
# with it" — and vkd3d-proton in a release Proton validates root parameter indices and
# types by assert() only, so it fails silently. This closes that gap for the harness.
#
# CI stays the primary gate. This is the supplementary one.
set -euo pipefail

HOST="${HOST:-root@192.168.0.210}"
PROTON_DIR="/home/deck/.local/share/Steam/compatibilitytools.d/GE-Proton-dxvk301-ds5-clean-nowl"
PREFIX="/home/deck/.local/share/Steam/steamapps/compatdata/1332010"
REMOTE_DIR="/tmp/stray-dlss-harness"
EXE="${1:-}"

if [[ -z "$EXE" || ! -f "$EXE" ]]; then
	echo "usage: $0 <path-to-stray_dlss_warp.exe>" >&2
	exit 2
fi

# The owner has authorised running alongside a live game. Still say so, loudly: the Wine
# prefix and the wineserver are shared with the running title, so a harness crash is not
# guaranteed to stay contained. Set STRICT=1 to refuse instead.
if ssh -o ConnectTimeout=8 "$HOST" 'pgrep -f "Stray-Win64-Shipping.exe" >/dev/null'; then
	if [[ "${STRICT:-0}" == "1" ]]; then
		echo "REFUSING (STRICT=1): Stray is running." >&2
		exit 1
	fi
	echo "NOTE: Stray is running; sharing its Wine prefix. Authorised by the owner."
fi

echo "==> copying harness to $HOST:$REMOTE_DIR"
ssh -o ConnectTimeout=8 "$HOST" "mkdir -p $REMOTE_DIR && chown -R deck:deck $REMOTE_DIR"
scp -q "$EXE" "$HOST:$REMOTE_DIR/harness.exe"
ssh "$HOST" "chown deck:deck $REMOTE_DIR/harness.exe"

echo "==> running under $(basename "$PROTON_DIR") against the real adapter"
# VKD3D_DEBUG=warn surfaces the FIXMEs and unsupported paths vkd3d-proton would otherwise
# swallow. Vulkan validation is left off by default: vkd3d-proton is not validation-clean,
# so it drowns the signal. Set VALIDATE=1 to turn it on when chasing something specific.
VALIDATION_ENV=""
if [[ "${VALIDATE:-0}" == "1" ]]; then
	VALIDATION_ENV='VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation'
fi

ssh "$HOST" "sudo -u deck env \
	HOME=/home/deck \
	STEAM_COMPAT_CLIENT_INSTALL_PATH=/home/deck/.local/share/Steam \
	STEAM_COMPAT_DATA_PATH=$PREFIX \
	VKD3D_DEBUG=warn \
	DXVK_LOG_LEVEL=warn \
	PROTON_LOG=0 \
	$VALIDATION_ENV \
	'$PROTON_DIR/proton' run '$REMOTE_DIR/harness.exe' --hardware 2>&1" | tee /tmp/harness-hw.log

echo
if grep -qE "^(FAIL|.*failures)" /tmp/harness-hw.log; then
	grep -E "PASS|FAIL|failures" /tmp/harness-hw.log | tail -3
fi
