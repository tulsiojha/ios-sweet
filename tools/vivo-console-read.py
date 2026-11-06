#!/usr/bin/env python3
"""Read the stage0 CDC-ACM console from macOS.

    vivo-console-read.py [SECONDS] [-o FILE]

macOS blocks on plain tty opens for this device and needs raw mode with
O_NONBLOCK, so a bare `cat /dev/tty.usbmodem*` hangs. This opens the
port nonblocking, switches it to raw and prints whatever arrives; with a
duration it exits after that many seconds (default: run until ^C).

The 5 s SET_LINE_CODING replay throttle means opening the port prints
the payload's boot summary line again, which makes this also a liveness
check.
"""
import argparse
import glob
import os
import select
import sys
import termios
import time


def find_port():
    ports = glob.glob("/dev/tty.usbmodemY21L*")
    if not ports:
        sys.exit("no /dev/tty.usbmodemY21L* port found")
    return ports[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("seconds", nargs="?", type=float, default=0)
    ap.add_argument("-o", "--out")
    ap.add_argument("-p", "--port")
    a = ap.parse_args()

    port = a.port or find_port()
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    attrs[0] = attrs[1] = attrs[2] = 0  # iflag, oflag, cflag: raw
    termios.tcsetattr(fd, termios.TCSANOW, attrs)

    out = open(a.out, "wb") if a.out else None
    t0 = time.time()
    total = 0
    try:
        while True:
            if a.seconds and time.time() - t0 >= a.seconds:
                break
            r, _, _ = select.select([fd], [], [], 0.5)
            if not r:
                continue
            d = os.read(fd, 65536)
            if not d:
                continue
            total += len(d)
            sys.stdout.write(d.decode("latin1"))
            sys.stdout.flush()
            if out:
                out.write(d)
                out.flush()
    except KeyboardInterrupt:
        pass
    finally:
        os.close(fd)
        if out:
            out.close()
    sys.stderr.write(f"[{total} bytes from {port}]\n")


if __name__ == "__main__":
    main()