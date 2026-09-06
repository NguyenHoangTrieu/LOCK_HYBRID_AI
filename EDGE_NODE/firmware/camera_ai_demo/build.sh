#!/usr/bin/env bash
# =========================================================================
# FRDM-MCXN947  -  build (west/CMake) + flash over MCU-Link (CMSIS-DAP)
#
# Copied from ../../touch_rgb/build.sh (same board, same probe setup) and
# retargeted at this app's name/paths.
#
#   ./build.sh            build, then flash
#   ./build.sh build      build only
#   ./build.sh rebuild    clean build from scratch
#   ./build.sh flash      flash the last build
#   ./build.sh clean      remove build/
#   ./build.sh erase      mass-erase the chip (recovery)
#   ./build.sh reset      reset the board without reflashing
#   ./build.sh monitor    open the serial console
#
# Dual-core RTOS migration (DUALCORE_RTOS=ON, see WORKLOG.md) - separate
# build dir (build_dualcore/) and commands, so the legacy single-core
# artifacts above are never at risk:
#   ./build.sh dualcore-build    build core1 then core0 (core0 embeds core1)
#   ./build.sh dualcore-flash    flash core0's combined image (one flash op)
#   ./build.sh dualcore-all      both of the above
# =========================================================================
set -euo pipefail

# ---- config -------------------------------------------------------------
BOARD="frdmmcxn947"
CORE="cm33_core0"
TARGET="mcxn947"              # pyOCD target name (from the CMSIS pack)
TOOLCHAIN="armgcc"
SERIAL_PORT="${SERIAL_PORT:-/dev/ttyACM0}"
BAUD=115200

APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WS_ROOT="$(cd "$APP_DIR/../../.." && pwd)"      # NPX_Workspace (Camera_AI_Test1/firmware/camera_ai_demo -> up 3)
SDK_WEST_ROOT="$WS_ROOT/mcuxsdk"                # holds .west/
BUILD_DIR="$APP_DIR/build"
DUALCORE_BUILD_DIR="$APP_DIR/build_dualcore"
VENV_BIN="$WS_ROOT/tools/westenv/bin"

export ARMGCC_DIR="${ARMGCC_DIR:-/usr}"
export PATH="$VENV_BIN:$PATH"

say() { printf '\033[1;36m>> %s\033[0m\n' "$*"; }
die() { printf '\033[1;31m!! %s\033[0m\n' "$*" >&2; exit 1; }

find_elf() {
	local f="$BUILD_DIR/camera_ai_demo_${CORE}.elf"
	[ -f "$f" ] && { echo "$f"; return 0; }
	f="$(find "$BUILD_DIR" -maxdepth 1 -name '*.elf' | head -1)"
	[ -n "$f" ] && { echo "$f"; return 0; }
	return 1
}

mcu_link_uid() {
	"$@" json --probes 2>/dev/null | "$VENV_BIN/python" -c '
import json, sys
try:
    data = json.load(sys.stdin)
except Exception:
    sys.exit(0)
for board in data.get("boards", []):
    if "MCU-LINK" in (board.get("product_name") or "").upper():
        print(board.get("unique_id", ""))
        break
'
}

pyocd_run() {
	local runner=("$VENV_BIN/pyocd")

	if ! "$VENV_BIN/pyocd" list 2>/dev/null | grep -qi "cmsis-dap"; then
		say "Probe not accessible as your user - using sudo."
		runner=(sudo env HOME="$HOME" "$VENV_BIN/pyocd")
	fi

	local uid; uid="$(mcu_link_uid "${runner[@]}")"
	if [ -n "$uid" ]; then
		"${runner[@]}" "$@" -u "$uid"
	else
		"${runner[@]}" "$@"
	fi
}

do_build() {
	[ -d "$SDK_WEST_ROOT/.west" ] || die "west workspace not found at $SDK_WEST_ROOT"
	say "Building $APP_DIR for $BOARD ($CORE)"
	( cd "$SDK_WEST_ROOT" && \
	  west build -b "$BOARD" "$APP_DIR" --toolchain "$TOOLCHAIN" \
	             -Dcore_id="$CORE" -d "$BUILD_DIR" "$@" )
	local elf; elf="$(find_elf)" || die "build finished but no .elf found"
	say "Built: $elf ($(du -h "$elf" | cut -f1))"
}

do_flash() {
	local elf; elf="$(find_elf)" || die "nothing to flash - run ./build.sh build first"
	say "Flashing $elf -> $TARGET via MCU-Link (CMSIS-DAP)"
	pyocd_run flash -t "$TARGET" "$elf"
	say "Done. Board reset and running."
}

do_erase() {
	say "Mass-erasing $TARGET"
	pyocd_run erase -t "$TARGET" --chip
}

do_reset() {
	say "Resetting board"
	pyocd_run reset -t "$TARGET"
}

do_monitor() {
	command -v tio     >/dev/null && exec tio "$SERIAL_PORT"
	command -v picocom >/dev/null && exec picocom -b "$BAUD" "$SERIAL_PORT"
	command -v screen  >/dev/null && exec screen "$SERIAL_PORT" "$BAUD"
	die "install tio, picocom or screen to use monitor"
}

# core1 MUST build before core0 - core0's link step .incbin's core1's raw
# .bin (fsl_incbin.S, see board_port/cm33_core0/app.h's comment), which
# requires core1's build dir to already exist and be findable at
# "../core1/armgcc/" relative to core0's own build dir (see CMakeLists.txt's
# DUALCORE_RTOS branch) - hence the fixed core0/core1 subdirectory names
# below, not just any -d path.
do_dualcore_build() {
	[ -d "$SDK_WEST_ROOT/.west" ] || die "west workspace not found at $SDK_WEST_ROOT"

	say "Building core1 (dual-core RTOS, see WORKLOG.md)"
	( cd "$SDK_WEST_ROOT" && \
	  west build -b "$BOARD" "$APP_DIR" --toolchain "$TOOLCHAIN" \
	             -Dcore_id=cm33_core1 -DDUALCORE_RTOS=ON \
	             -d "$DUALCORE_BUILD_DIR/core1" )

	# CMake/Ninja has no idea fsl_incbin.S's `.incbin "core1_image.bin"`
	# depends on core1's binary (it's an assembler-time file read, not a
	# tracked build dependency) - so core0 silently keeps re-linking a
	# STALE embedded core1 image if core1 changed but core0's own sources
	# didn't (confirmed the hard way: core1 rebuilt with a real fix, core0's
	# .elf mtime didn't move, board kept running the old core1 image).
	# Force it to always re-embed by deleting its one cached object file
	# before every core0 build - cheap (one file, one relink), and the only
	# way to guarantee core0 never ships a stale core1 image.
	find "$DUALCORE_BUILD_DIR/core0" -name 'fsl_incbin.S.obj*' -delete 2>/dev/null || true

	say "Building core0 (embeds core1's image)"
	( cd "$SDK_WEST_ROOT" && \
	  west build -b "$BOARD" "$APP_DIR" --toolchain "$TOOLCHAIN" \
	             -Dcore_id=cm33_core0 -DDUALCORE_RTOS=ON \
	             -d "$DUALCORE_BUILD_DIR/core0" )

	local elf="$DUALCORE_BUILD_DIR/core0/camera_ai_demo_cm33_core0.elf"
	[ -f "$elf" ] || die "dual-core build finished but no combined .elf found at $elf"
	say "Built: $elf ($(du -h "$elf" | cut -f1)) - core1 embedded inside, one flash op needed"
}

do_dualcore_flash() {
	local elf="$DUALCORE_BUILD_DIR/core0/camera_ai_demo_cm33_core0.elf"
	[ -f "$elf" ] || die "nothing to flash - run ./build.sh dualcore-build first"
	say "Flashing $elf -> $TARGET via MCU-Link (CMSIS-DAP)"
	pyocd_run flash -t "$TARGET" "$elf"
	say "Done. Board reset and running - core0 boots, then releases core1."
}

case "${1:-all}" in
	build)          shift; do_build "$@";;
	rebuild)        shift; rm -rf "$BUILD_DIR"; do_build "$@";;
	flash)          do_flash;;
	erase)          do_erase;;
	reset)          do_reset;;
	clean)          say "removing $BUILD_DIR"; rm -rf "$BUILD_DIR";;
	monitor)        do_monitor;;
	all)            do_build; do_flash;;
	dualcore-build) do_dualcore_build;;
	dualcore-flash) do_dualcore_flash;;
	dualcore-all)   do_dualcore_build; do_dualcore_flash;;
	*)       die "unknown command '$1' (build|rebuild|flash|erase|reset|clean|monitor|dualcore-build|dualcore-flash|dualcore-all)";;
esac
