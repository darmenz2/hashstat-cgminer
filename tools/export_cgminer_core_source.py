#!/usr/bin/env python3

from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import json
from pathlib import Path
import tarfile

from build_cgminer_core import ROOT, digest, metadata, source_inputs, verify_portable


def export(source: Path, lock_path: Path, output: Path) -> dict:
    source = source.resolve(strict=True)
    lock_path = lock_path.resolve(strict=True)
    if not output.parent.is_dir() or output.exists() or output.is_symlink():
        raise ValueError("output must be new under an existing parent")
    receipt_path = Path(str(output) + ".receipt.json")
    if receipt_path.exists() or receipt_path.is_symlink():
        raise ValueError("output receipt must be new")
    if any(item.resolve().is_relative_to(root) for item in (output, receipt_path) for root in (source, ROOT.resolve())):
        raise ValueError("output must be outside source inputs")
    rows, epoch, lock = source_inputs(source, lock_path)
    verify_portable(source, lock)
    lock_bytes = lock_path.read_bytes()
    if json.loads(lock_bytes) != lock:
        raise ValueError("source lock changed during validation")
    lock_sha = hashlib.sha256(lock_bytes).hexdigest()
    build_rows = lock["supportFiles"]
    build_names = [row["path"] for row in build_rows]
    if (not build_names or build_names != sorted(set(build_names))
            or any(name.startswith("core/") or name in ("source-lock.json", "build-inputs.json") for name in build_names)
            or metadata(build_names, ROOT) != build_rows):
        raise ValueError("support source differs from reviewed lock")
    inputs = {"schema": "hashstat.cgminer-core-build-inputs.v1", "scope": "source-built core only",
              "sourceLockSHA256": lock_sha, "buildFiles": build_rows,
              "wholeFirmwareSourceComplete": False, "vendorELFIncluded": False}
    payloads = [("core/" + row["path"], source / row["path"], row) for row in rows]
    payloads += [(row["path"], ROOT / row["path"], row) for row in build_rows]
    payloads += [("source-lock.json", lock_path,
                  {"size": len(lock_bytes), "executable": False, "sha256": lock_sha})]
    with output.open("xb") as raw, gzip.GzipFile(filename="", fileobj=raw, mode="wb", mtime=int(epoch)) as zipped:
        with tarfile.open(fileobj=zipped, mode="w|", format=tarfile.PAX_FORMAT) as archive:
            for name, path, row in sorted(payloads):
                info = tarfile.TarInfo("hashstat-core-source/" + name)
                info.size = row["size"]
                info.mode = 0o755 if row["executable"] else 0o644
                info.mtime = int(epoch)
                data = path.read_bytes()
                if len(data) != row["size"] or hashlib.sha256(data).hexdigest() != row["sha256"]:
                    raise ValueError("source changed while reading archive payload")
                archive.addfile(info, io.BytesIO(data))
            data = (json.dumps(inputs, sort_keys=True, indent=2) + "\n").encode()
            info = tarfile.TarInfo("hashstat-core-source/build-inputs.json")
            info.size, info.mode, info.mtime = len(data), 0o644, int(epoch)
            archive.addfile(info, io.BytesIO(data))

    if metadata([row["path"] for row in rows], source) != rows or digest(lock_path) != lock_sha:
        raise ValueError("source/lock changed during export; retained archive is NOT accepted")
    if metadata(build_names, ROOT) != build_rows:
        raise ValueError("build tools changed during export; retained archive is NOT accepted")
    receipt = {"status": "LOCKED_CORE_SOURCE_EXPORTED", "archive": str(output.resolve()),
               "sha256": digest(output), "size": output.stat().st_size,
               "sourceLockSHA256": lock_sha, "sourceFiles": len(rows), "buildFiles": len(build_rows),
               "sourceUnchanged": True, "wholeFirmwareSourceComplete": False,
               "compilerIncluded": False, "built": False, "installed": False, "published": False}
    with receipt_path.open("x") as stream:
        json.dump(receipt, stream, sort_keys=True, indent=2)
        stream.write("\n")
    return receipt


def main() -> None:
    parser = argparse.ArgumentParser(description=None)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--source-lock", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(export(args.source, args.source_lock, args.output), sort_keys=True))


if __name__ == "__main__":
    main()
