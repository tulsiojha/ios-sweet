#!/usr/bin/env python3
"""Watch the vivo Y21L CDC-ACM console across re-enumeration.

    vivo-console-watch.py [SECONDS] [-o FILE] [--pattern GLOB]

Unlike vivo-console-read.py this keeps running when the tty disappears, so it
can be started before a reboot (when no tty exists yet) and it logs the moment
the port opens or closes. That matters because the stage0 payload is replaced by
xnu mid-run: a USBCMD.RESET deasserts D+ and macOS reports the close, then the
kernel's own console enumerates under a new name ("vivo Y21L xnu").

Started before `fastboot reboot`, it captures the stage0 boot ring from t=0.
Raw + O_NONBLOCK is required on this macOS (a plain open blocks).
"""
import argparse
import glob
import os
import select
import sys
import termios
import time


def stamp():
    return time.strftime("%H:%M:%S")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("seconds", nargs="?", type=float, default=0)
    ap.add_argument("-o", "--out")
    ap.add_argument("-t", "--timeline", help="write one line per read chunk")
    ap.add_argument("--pattern", default="/dev/tty.usbmodem*")
    a = ap.parse_args()

    out = open(a.out, "ab") if a.out else None
    tl = open(a.timeline, "a") if a.timeline else None
    t0 = time.time()
    total = 0
    fd = None
    port = None

    def emit(s):
        sys.stdout.write(s)
        sys.stdout.flush()
        if out:
            out.write(s.encode("latin1"))
            out.flush()

    def note(s):
        emit(s)
        if tl:
            tl.write(f"{time.time():.3f} {s.strip()}\n")
            tl.flush()

    try:
        while True:
            if a.seconds and time.time() - t0 >= a.seconds:
                break

            if fd is None:
                ports = sorted(glob.glob(a.pattern))
                if not ports:
                    time.sleep(0.1)
                    continue
                port = ports[0]
                try:
                    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
                except OSError:
                    fd = None
                    time.sleep(0.1)
                    continue
                attrs = termios.tcgetattr(fd)
                attrs[0] = attrs[1] = attrs[2] = 0  # iflag, oflag, cflag: raw
                termios.tcsetattr(fd, termios.TCSANOW, attrs)
                note(f"\n[{stamp()}] open {port}\n")
                continue

            r, _, _ = select.select([fd], [], [], 0.5)
            if not r:
                # A vanished node is not always an error; check the glob too.
                if port not in glob.glob(a.pattern):
                    try:
                        d = os.read(fd, 65536)
                    except OSError:
                        d = b""
                    if d:
                        total += len(d)
                        emit(d.decode("latin1"))
                    note(f"\n[{stamp()}] closed {port} (node gone)\n")
                    os.close(fd)
                    fd = None
                continue
            try:
                d = os.read(fd, 65536)
            except OSError:
                note(f"\n[{stamp()}] closed {port} (read error)\n")
                os.close(fd)
                fd = None
                continue
            if not d:
                continue
            total += len(d)
            if tl:
                last = d.decode("latin1").rstrip("\r\n").split("\n")[-1][:60]
                tl.write(f"{time.time():.3f} +{len(d)} {last}\n")
                tl.flush()
            emit(d.decode("latin1"))
    except KeyboardInterrupt:
        pass
    finally:
        if fd is not None:
            os.close(fd)
        if out:
            out.close()
        if tl:
            tl.close()
    sys.stderr.write(f"[{total} bytes, {time.time() - t0:.0f} s]\n")


if __name__ == "__main__":
    main()
