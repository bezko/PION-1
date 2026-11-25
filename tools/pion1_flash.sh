#!/usr/bin/env bash
#
# PION/1 -- flash the firmware and wait for the console to come up.
#
# The point of this script is to make the whole sequence repeatable.  It
# handles the parts that are easy to get wrong by hand:
#
#   1. waiting for the Pico to appear in BOOTSEL mode (mass storage),
#   2. copying the UF2, and waiting for the volume to be re-enumerated
#      rather than assuming the copy had taken effect,
#   3. waiting for the application to come back as a USB CDC device,
#   4. printing the stable /dev/serial/by-id path to talk to.
#
# If the Pico is running an application that has no USB interface -- which
# is the case for an older PION/1 build, or for any program without USB
# support -- then it is invisible to the host and the only way in is
# BOOTSEL mode.  Hold the BOOTSEL button while pressing RESET (or while
# plugging the cable in), and this script will take it from there.
#
# Usage:
#   tools/pion1_flash.sh                  # build, flash, wait for the CDC
#   tools/pion1_flash.sh --no-build       # flash the existing build
#   tools/pion1_flash.sh --reboot         # reset into BOOTSEL and wait
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${ROOT}/build"
UF2="${BUILD}/PION1.uf2"
DO_BUILD=1
REBOOT=0

for arg in "$@"; do
    case "$arg" in
        --no-build) DO_BUILD=0 ;;
        --reboot)   REBOOT=1 ;;
        -h|--help)  sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $arg" >&2; exit 1 ;;
    esac
done

say() { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!!\033[0m %s\n' "$*" >&2; }
die() { printf '\033[1;31mxx\033[0m %s\n' "$*" >&2; exit 1; }

# --- 1. build ---------------------------------------------------------------
if [ "$DO_BUILD" -eq 1 ]; then
    say "building"
    cmake --build "$BUILD" -j"$(nproc)" >/dev/null
fi

[ -f "$UF2" ] || die "no UF2 at $UF2"

# --- 2. get into BOOTSEL mode ------------------------------------------------
if [ "$REBOOT" -eq 1 ]; then
    say "asking the Pico to reboot into BOOTSEL"
    # The reset interface is exposed by pico_stdio_usb, so a running
    # application can be asked to drop into the bootloader over USB.
    picotool reboot -u -f || warn "could not ask for a reboot; use the BOOTSEL button"
fi

say "waiting for the Pico in BOOTSEL mode"
BOOT_TIMEOUT=60
for _ in $(seq 1 $((BOOT_TIMEOUT * 2))); do
    if [ -d /media/"$(id -un)"/RPI-RP2 ]; then
        break
    fi
    if lsusb 2>/dev/null | grep -qi "2e8a:0003"; then
        sleep 1   # the volume takes a moment to be mounted
        [ -d /media/"$(id -un)"/RPI-RP2 ] && break
    fi
    sleep 0.5
done

MOUNT="/media/$(id -un)/RPI-RP2"
if [ ! -d "$MOUNT" ]; then
    warn "the Pico did not appear in BOOTSEL mode within ${BOOT_TIMEOUT}s."
    warn "Hold the BOOTSEL button while pressing RESET, then run this again."
    exit 1
fi

# --- 3. copy the UF2 --------------------------------------------------------
say "flashing $(basename "$UF2")"
cp "$UF2" "$MOUNT/"
sync

# The bootloader ejects the volume and reboots as soon as the file has
# been written.  The copy above is enough to trigger it, but the "lost
# async page write" error in dmesg is normal -- the data is already in
# the flash by then.
for _ in $(seq 1 40); do
    [ -d "$MOUNT" ] || break
    sleep 0.5
done

# --- 4. wait for the application to come back as a CDC ----------------------
say "waiting for the USB CDC"
CDC_TIMEOUT=30
BY_ID=""
for _ in $(seq 1 $((CDC_TIMEOUT * 2))); do
    BY_ID=$(ls /dev/serial/by-id/*PION-1* 2>/dev/null | head -1 || true)
    [ -n "$BY_ID" ] && break
    if lsusb 2>/dev/null | grep -qi "2e8a:000a"; then
        BY_ID=$(ls /dev/serial/by-id/*PION-1* 2>/dev/null | head -1 || true)
        [ -n "$BY_ID" ] && break
    fi
    sleep 0.5
done

if [ -z "$BY_ID" ]; then
    warn "the Pico rebooted but no CDC device appeared within ${CDC_TIMEOUT}s."
    warn "Check dmesg for USB errors; a charge-only cable is the usual cause."
    exit 1
fi

say "ready"
echo
echo "  console:  $BY_ID"
echo "  talk to:  python3 tools/pion1_console.py"
echo "  or:       minicom -D $BY_ID -b 115200"
echo
