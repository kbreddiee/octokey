#!/usr/bin/env python3
"""Wrap a merged dongle image into an espkvm dongle pack.

The pack is what the hub stores in its `dimage` flash partition and
streams into a T-Dongle-S3 plugged into its USB port (see
firmware/hub/main/flasher.c). Layout: 32-byte header + payload.

    header: magic 'KVMD' (LE u32), payload_len u32, crc32 u32,
            flash_addr u32 (0 for merged images), fw_ver u16, 14 pad bytes

Usage:
    gen_dongle_pack.py --image espkvm-dongle-s3-web.bin \
                       --out espkvm-dongle-pack.bin [--fw-ver 1]

Flash it to a hub at the dimage partition offset:
    esptool.py --chip esp32s3 write_flash 0x290000 espkvm-dongle-pack.bin
"""

import argparse
import pathlib
import struct
import zlib

MAGIC = 0x444D564B          # "KVMD" little-endian
HDR_FMT = "<IIIIH14x"       # 32 bytes
DIMAGE_PART_SIZE = 0x100000


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--image", required=True,
                    help="merged dongle image (flashed at offset 0)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--fw-ver", type=int, default=1)
    ap.add_argument("--flash-addr", type=lambda x: int(x, 0), default=0)
    args = ap.parse_args()

    payload = pathlib.Path(args.image).read_bytes()
    if len(payload) + 32 > DIMAGE_PART_SIZE:
        raise SystemExit(
            f"image is {len(payload)} bytes; the dimage partition holds "
            f"{DIMAGE_PART_SIZE - 32} — enlarge the partition or slim the app")

    hdr = struct.pack(HDR_FMT, MAGIC, len(payload),
                      zlib.crc32(payload) & 0xFFFFFFFF,
                      args.flash_addr, args.fw_ver)
    out = pathlib.Path(args.out)
    out.write_bytes(hdr + payload)
    print(f"wrote {out} ({len(payload)} payload bytes, fw v{args.fw_ver})")


if __name__ == "__main__":
    main()
