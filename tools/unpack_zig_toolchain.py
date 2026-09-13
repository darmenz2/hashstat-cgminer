#!/usr/bin/env python3

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import tarfile


def check(ok, reason):
    if not ok:
        raise ValueError(reason)


def main():
    ap = argparse.ArgumentParser(description=None)
    ap.add_argument("--archive", type=Path, required=True)
    ap.add_argument("--parent", type=Path, required=True)
    ap.add_argument("--lock", type=Path,
                    help="reviewed host-specific archive lock; defaults to the original macOS lock")
    args = ap.parse_args()
    check(args.archive.is_absolute() and args.archive.is_file() and not args.archive.is_symlink(), "regular absolute archive required")
    check(args.parent.is_absolute() and args.parent.is_dir() and not args.parent.is_symlink(), "regular absolute parent required")
    root = Path(__file__).resolve().parent.parent
    lock_path = args.lock or root / "toolchains/zig-0.16.0.lock.json"
    lock = json.loads(lock_path.read_text())
    check(args.archive.stat().st_size == lock["size"], "archive size mismatch")
    with args.archive.open("rb") as stream:
        h = hashlib.sha256()
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    check(h.hexdigest() == lock["sha256"], "official archive hash mismatch")
    prefix = "zig-" + lock["host"] + "-" + lock["version"]
    check(PurePosixPath(prefix).parts == (prefix,) and "\\" not in prefix and
          not any(ord(c) < 32 for c in prefix), "unsafe locked archive prefix")
    target = args.parent / prefix
    check(not target.exists() and not target.is_symlink(), "refuse existing extraction tree")
    receipt_path = args.parent / (prefix + "-extraction-receipt.json" if args.lock
                                 else "zig-extraction-receipt.json")
    check(not receipt_path.exists() and not receipt_path.is_symlink(), "refuse existing extraction receipt")
    with tarfile.open(args.archive, "r:xz") as archive:
        entries = archive.getmembers()
        check(0 < len(entries) < 100000, "invalid entry count")
        total = 0
        names = set()
        for item in entries:
            path = PurePosixPath(item.name)
            check(not path.is_absolute() and path.parts and path.parts[0] == prefix and
                  ".." not in path.parts and "\\" not in item.name and "\0" not in item.name,
                  "unsafe archive name")
            check(item.isdir() or item.isfile(), "links and special files forbidden")
            check(item.name.rstrip("/") not in names, "duplicate entry")
            names.add(item.name.rstrip("/"))
            check(0 <= item.size <= 512 * 1024 * 1024, "oversized entry")
            total += item.size
        check(total < 1024 * 1024 * 1024, "unpacked resource limit")

        regular = {item.name.rstrip("/") for item in entries if item.isfile()}
        for item in entries:
            check(not any(str(p) in regular for p in PurePosixPath(item.name).parents), "file/directory collision")
        target.mkdir(mode=0o700)
        for item in entries:
            destination = args.parent.joinpath(*PurePosixPath(item.name).parts)
            if item.isdir():
                destination.mkdir(mode=0o700, parents=True, exist_ok=True)
                continue
            destination.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
            source = archive.extractfile(item)
            check(source is not None, "missing data")
            with source, destination.open("xb") as output:
                shutil.copyfileobj(source, output, length=1024 * 1024)
            check(destination.stat().st_size == item.size, "extracted size mismatch")
            os.chmod(destination, 0o700 if item.mode & 0o111 else 0o600)
    receipt = {"status": "VERIFIED_AND_EXTRACTED_NOT_EXECUTED", "archiveSHA256": h.hexdigest(),
               "entries": len(entries), "unpackedBytes": total, "directory": str(target),
               "privateDataIncluded": False, "systemInstall": False}
    with receipt_path.open("x") as output:
        output.write(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps(receipt))


if __name__ == "__main__":
    main()
