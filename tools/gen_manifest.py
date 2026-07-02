#!/usr/bin/env python3
"""Generate an ESP Web Tools manifest for an espkvm firmware image.

ESP Web Tools (https://esphome.github.io/esp-web-tools/) lets people flash
firmware from Chrome/Edge with no toolchain installed. It needs a manifest
JSON pointing at a *merged* image (bootloader + partition table + app at
offset 0). CI produces that merged image with:

    esptool.py --chip <target> merge_bin -o espkvm-<name>-web.bin @flash_args

Usage:
    gen_manifest.py --name hub    --chip ESP32-S3 --version 1.0.0 --out build/
    gen_manifest.py --name dongle --chip ESP32-S2 --version 1.0.0 --out build/
"""

import argparse
import json
import pathlib


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--name", required=True,
                    choices=["hub", "hub-tembed", "dongle", "dongle-s3"])
    ap.add_argument("--chip", required=True,
                    choices=["ESP32-S2", "ESP32-S3"])
    ap.add_argument("--version", default="dev")
    ap.add_argument("--out", default=".", help="output directory")
    args = ap.parse_args()

    manifest = {
        "name": f"espkvm {args.name}",
        "version": args.version,
        "new_install_prompt_erase": True,
        "builds": [
            {
                "chipFamily": args.chip,
                "parts": [
                    {"path": f"espkvm-{args.name}-web.bin", "offset": 0}
                ],
            }
        ],
    }

    out = pathlib.Path(args.out) / f"manifest-{args.name}.json"
    out.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
