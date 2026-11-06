#!/usr/bin/env python3
"""Repack the vivo Y21L stock boot.img with a replacement kernel blob.

    vivo-repack-boot.py STOCK_BOOT KERNEL_BLOB OUT [--cmdline "..."]
                        [--ramdisk-blob FILE [--grow-ramdisk]]

Keeps everything from the stock image (header addresses, ramdisk, QCDT
DTB area, appended PKCS#7 block) and only swaps the kernel region. The
new blob is zero-padded to the original kernel_size so that every later
offset (ramdisk at 0x82000000, QCDT at +0x3000) stays exactly where LK
expects it. The blob must be raw ARM32 code entered at kernel_addr.

--ramdisk-blob writes FILE into the stock ramdisk region (LK loads it to
ramdisk_addr, 0x82000000) zero-padded to the stock ramdisk_size, so the
header and every offset stay untouched; FILE must fit in that region.
stage0's SWEETXNU payload is injected this way.

--grow-ramdisk allows FILE to be larger than the stock region: the header
ramdisk_size is raised to the blob size page-aligned, and the QCDT area
plus any appended data are moved after it. The image gets bigger (a 10 MB
xnu payload yields ~29 MB), so the boot partition must have room.
"""
import argparse
import hashlib
import struct
import sys

PAGE = 0x800  # stock page_size


def cstr(b):
    return b.split(b"\0", 1)[0].decode("latin1")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("stock")
    ap.add_argument("kernel")
    ap.add_argument("out")
    ap.add_argument("--cmdline")
    ap.add_argument("--ramdisk-blob", help="SWEETXNU payload for the ramdisk slot")
    ap.add_argument("--grow-ramdisk", action="store_true",
                    help="raise ramdisk_size and move the QCDT after a bigger blob")
    a = ap.parse_args()

    d = bytearray(open(a.stock, "rb").read())
    if d[:8] != b"ANDROID!":
        sys.exit("not an Android boot image")
    (ker_sz, ker_addr, rd_sz, rd_addr, sec_sz, sec_addr,
     tags_addr, page, dt_sz) = struct.unpack_from("<9I", d, 8)
    if page != PAGE:
        sys.exit(f"unexpected page size {page:#x}")

    blob = open(a.kernel, "rb").read()
    if len(blob) > ker_sz:
        sys.exit(f"blob {len(blob)} > stock kernel region {ker_sz}")
    print(f"stock: kernel {ker_sz} @ {ker_addr:#x}, ramdisk {rd_sz} @ {rd_addr:#x}, "
          f"dt {dt_sz}, page {page}, cmdline {cstr(d[0x48:0x248])!r}")

    d[page:page + ker_sz] = blob + b"\0" * (ker_sz - len(blob))
    rd_off = (page + ker_sz + page - 1) & ~(page - 1)	# sections are page aligned
    rd_note = ""
    if a.ramdisk_blob:
        rd = open(a.ramdisk_blob, "rb").read()
        if len(rd) > rd_sz:
            if not a.grow_ramdisk:
                sys.exit(f"ramdisk blob {len(rd)} > stock ramdisk region {rd_sz}")
            new_rd_sz = (len(rd) + PAGE - 1) & ~(PAGE - 1)
            struct.pack_into("<I", d, 16, new_rd_sz)
            dt_off = (rd_off + rd_sz + PAGE - 1) & ~(PAGE - 1)
            new_dt_off = (rd_off + new_rd_sz + PAGE - 1) & ~(PAGE - 1)
            dt = bytes(d[dt_off:dt_off + dt_sz])
            tail = bytes(d[dt_off + dt_sz:])
            d = d[:page + ker_sz]
            d += b"\0" * (rd_off - len(d))
            d += rd + b"\0" * (new_rd_sz - len(rd))
            d += b"\0" * (new_dt_off - len(d))
            d += dt + tail
            rd_note = (f", ramdisk blob {len(rd)} bytes @ {rd_off:#x} "
                       f"padded to {new_rd_sz} (grown from {rd_sz}), "
                       f"dt moved {dt_off:#x} -> {new_dt_off:#x}")
        else:
            d[rd_off:rd_off + rd_sz] = rd + b"\0" * (rd_sz - len(rd))
            rd_note = f", ramdisk blob {len(rd)} bytes @ {rd_off:#x} padded to {rd_sz}"
    if a.cmdline is not None:
        c = a.cmdline.encode()
        if len(c) >= 512:
            sys.exit("cmdline too long")
        d[0x48:0x248] = c + b"\0" * (512 - len(c))

    open(a.out, "wb").write(d)
    print(f"wrote {a.out}: {len(d)} bytes, kernel {len(blob)} bytes padded to {ker_sz}{rd_note}")
    print("sha256", hashlib.sha256(d).hexdigest())


if __name__ == "__main__":
    main()
