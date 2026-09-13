#!/usr/bin/env python3

import argparse
import hashlib
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parent.parent
DESTINATION = ROOT / "core/hashstat-pure"
PORTABLE = (
    "include/hs_span.h",
    "src/pow.c", "include/hs_pow.h",
    "src/aml88_profile.c", "include/hs_aml88_profile.h",
    "src/job_cache.c", "include/hs_job_cache.h",
    "src/bm1362_rx.c", "include/hs_bm1362_rx.h",
    "src/btm_work_wire.c", "include/hs_btm_work_wire.h",
    "src/work.c", "include/hs_work.h",
    "src/miner_lifecycle.c", "include/hs_miner_lifecycle.h",
    "transport/runtime/aml_chain.c", "transport/include/hs_aml_chain.h",
    "transport/src/aml_uart.c", "transport/include/hs_aml_uart.h",
    "transport/src/bm1362_integrity.c", "transport/include/hs_bm1362_integrity.h",
)


def sha(path):
    if path.is_symlink() or not path.is_file():
        raise ValueError("regular source required: " + str(path))
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify():
    return {rel: sha(ROOT / rel) for rel in PORTABLE
            if sha(ROOT / rel) == sha(DESTINATION / Path(rel).name)}


def main():
    parser = argparse.ArgumentParser(description=None)
    parser.add_argument("--copy-new", action="store_true")
    args = parser.parse_args()
    if DESTINATION.is_symlink() or not DESTINATION.is_dir():
        raise ValueError("regular existing destination directory required")

    before = {rel: sha(ROOT / rel) for rel in PORTABLE}
    for rel, expected in before.items():
        target = DESTINATION / Path(rel).name
        if target.exists() or target.is_symlink():
            if sha(target) != expected:
                raise ValueError("modified portable copy; review first: " + str(target))
        elif not args.copy_new:
            raise ValueError("missing portable copy: " + str(target))
    if args.copy_new:
        for rel in PORTABLE:
            target = DESTINATION / Path(rel).name
            if not target.exists():
                with (ROOT / rel).open("rb") as source, target.open("xb") as out:
                    shutil.copyfileobj(source, out)
    if verify() != before:
        raise ValueError("portable source changed during export")
    print("VERIFIED_PORTABLE_FILES", len(before))


if __name__ == "__main__":
    main()
