#!/usr/bin/env python3
"""
PION/1 -- host console.

Opens the PION/1's USB CDC port, waits for the device to enumerate, and
prints what it says.  The point of this script is that the connection is
*repeatable*: the port is found by its stable /dev/serial/by-id symlink
rather than by whatever ttyACM number the kernel happened to hand out,
and the tool waits for the device instead of failing if it is not there
yet.

Usage:
    pion1-console                  # attach and print until Ctrl-C
    pion1-console --send '+'       # attach, send a key, print the reply
    pion1-console --list           # list the PION/1 ports and exit
    pion1-console --wait           # wait for the device, then exit

The by-id symlink is /dev/serial/by-id/usb-Raspberry_Pi_PION-1-if00, where
the "if00" suffix is the CDC interface number.  It is stable across
reboots, replugs and reorderings of other USB devices.
"""

import argparse
import glob
import os
import sys
import time

# udev names the by-id link from the USB product and manufacturer strings
# and the CDC interface number.  Match on the product so that other CDC
# devices on the same bus are not picked up.
BY_ID_GLOB = "/dev/serial/by-id/*PION-1*"

# How long to wait for the device to show up, in seconds.
DEFAULT_WAIT = 30.0

# How long to wait for the firmware's banner, in seconds.
BANNER_TIMEOUT = 10.0


def find_ports():
    """Return the list of PION/1 CDC ports, by-id first."""
    ports = sorted(glob.glob(BY_ID_GLOB))

    # Fall back to /dev/ttyACM* if the by-id link has not been created
    # yet (udev may lag a moment behind the kernel's enumeration).
    if not ports:
        ports = sorted(glob.glob("/dev/ttyACM*"))

    return ports


def read_serial(port):
    """Open the port and yield lines as they arrive."""
    import serial

    ser = serial.Serial(port, 115200, timeout=0.2)

    # The firmware prints its banner when it sees DTR asserted, so make
    # sure DTR is up before we start reading.  pyserial asserts DTR on
    # open, but be explicit about it anyway: this is the line that
    # decides whether the device thinks a host is attached.
    ser.dtr = True
    ser.rts = True
    ser.reset_input_buffer()

    try:
        buf = b""
        while True:
            chunk = ser.read(ser.in_waiting or 1)
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                yield line.decode("utf-8", "replace").rstrip("\r")
    finally:
        ser.close()


def wait_for_device(timeout):
    """Wait for a PION/1 to enumerate.  Returns the port path or None."""
    deadline = time.time() + timeout
    announced = False

    while time.time() < deadline:
        ports = find_ports()
        if ports:
            return ports[0]
        if not announced:
            print("Waiting for PION/1 to enumerate...", file=sys.stderr)
            announced = True
        time.sleep(0.25)

    return None


def main():
    ap = argparse.ArgumentParser(description="PION/1 host console")
    ap.add_argument("--port", help="serial port to use (default: auto-detect)")
    ap.add_argument("--send", help="a key to send once connected")
    ap.add_argument("--list", action="store_true",
                    help="list the PION/1 ports and exit")
    ap.add_argument("--wait", action="store_true",
                    help="wait for the device to appear, then exit")
    ap.add_argument("--timeout", type=float, default=DEFAULT_WAIT,
                    help="seconds to wait for the device (default: %(default)s)")
    args = ap.parse_args()

    if args.list:
        ports = find_ports()
        if not ports:
            print("no PION/1 found")
            return 1
        for p in ports:
            print(p)
        return 0

    if args.port:
        port = args.port
    else:
        port = wait_for_device(args.timeout)
        if port is None:
            print("no PION/1 found after %.0fs" % args.timeout, file=sys.stderr)
            return 1

    print("connected: %s" % port, file=sys.stderr)

    try:
        lines = read_serial(port)
    except Exception as exc:                       # noqa: BLE001
        print("cannot open %s: %s" % (port, exc), file=sys.stderr)
        return 1

    # Send the key first, if asked, so that we do not miss the reply.
    if args.send:
        import serial
        ser = serial.Serial(port, 115200, timeout=0.2)
        try:
            ser.dtr = True
            ser.write(args.send.encode())
            ser.flush()
        finally:
            ser.close()

    # Read until the banner arrives, then keep going until interrupted.
    deadline = time.time() + BANNER_TIMEOUT
    for line in lines:
        print(line)
        sys.stdout.flush()
        deadline = time.time() + BANNER_TIMEOUT

    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print()
        sys.exit(0)
