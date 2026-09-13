#!/usr/bin/env python3

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import stat
import tarfile


def require(value, message):
    if not value:
        raise ValueError(message)


def sha_stream(stream):
    result = hashlib.sha256()
    for block in iter(lambda: stream.read(1024 * 1024), b""):
        result.update(block)
    return result.hexdigest()


def sha_file(path):
    require(stat.S_ISREG(path.lstat().st_mode), "not a regular file: " + str(path))
    with path.open("rb") as stream:
        return sha_stream(stream)


def main():
    parser = argparse.ArgumentParser(description=None)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--lock", type=Path,
                        help="reviewed host-specific archive lock; defaults to the original macOS lock")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    lock_path = args.lock or root / "toolchains/zig-0.16.0.lock.json"
    lock = json.loads(lock_path.read_text())
    require(args.archive.is_absolute() and args.directory.is_absolute() and args.output.is_absolute(),
            "absolute paths required")
    require(args.directory.is_dir() and not args.directory.is_symlink(), "regular compiler root required")
    require(not args.output.exists() and not args.output.is_symlink() and args.output.parent.is_dir(),
            "output must be new under an existing directory")
    require(args.archive.stat().st_size == lock["size"] and sha_file(args.archive) == lock["sha256"],
            "official archive pin mismatch")
    prefix = "zig-" + lock["host"] + "-" + lock["version"]
    require(PurePosixPath(prefix).parts == (prefix,) and "\\" not in prefix and
            not any(ord(c) < 32 for c in prefix), "unsafe locked archive prefix")
    entries, seen, directories = [], set(), {"."}
    total = 0
    with tarfile.open(args.archive, "r:xz") as archive:
        for item in archive:
            path = PurePosixPath(item.name)
            require(path.parts and path.parts[0] == prefix and not path.is_absolute() and
                    ".." not in path.parts and "\\" not in item.name and
                    not any(ord(c) < 32 for c in item.name), "unsafe archive entry")
            relative = PurePosixPath(*path.parts[1:])
            name = str(relative)
            require(name not in seen, "duplicate archive entry")
            seen.add(name)
            require(len(seen) <= 100000 and (item.isfile() or item.isdir()), "unsupported entry")
            current = args.directory
            for component in relative.parts:
                current /= component
                require(not current.is_symlink(), "local compiler symlink: " + name)
            if item.isdir():
                require(current.is_dir(), "compiler directory absent: " + name)
                directories.add(name)
                continue
            total += item.size
            require(0 <= item.size <= 512 * 1024 * 1024 and total < 1024 * 1024 * 1024,
                    "compiler resource cap exceeded")
            mode = current.lstat().st_mode
            require(stat.S_ISREG(mode) and current.stat().st_size == item.size,
                    "local compiler entry/type/size mismatch: " + name)
            require(bool(mode & 0o111) == bool(item.mode & 0o111), "executable bit mismatch: " + name)
            incoming = archive.extractfile(item)
            require(incoming is not None, "missing archived data")
            with incoming:
                expected = sha_stream(incoming)
            require(sha_file(current) == expected, "local compiler content changed: " + name)
            entries.append({"path": name, "size": item.size, "executable": bool(item.mode & 0o111),
                            "sha256": expected})
            for parent in relative.parents:
                directories.add(str(parent))
    local = set()
    for parent, subdirs, files in os.walk(args.directory, followlinks=False):
        for name in subdirs + files:
            path = Path(parent) / name
            require(not path.is_symlink(), "extra compiler symlink: " + str(path))
            local.add(path.relative_to(args.directory).as_posix())
    expected_names = {item["path"] for item in entries} | (directories - {"."})
    require(local == expected_names, "unexpected/absent compiler tree entries: " +
            repr(sorted(local.symmetric_difference(expected_names))[:12]))
    entries.sort(key=lambda entry: entry["path"])
    canonical = json.dumps(entries, sort_keys=True, separators=(",", ":")).encode("utf-8")
    receipt = {"schema": "hashstat.toolchain-tree-verification.v1", "status": "PASSED",
               "lockSHA256": sha_file(lock_path), "archiveSHA256": lock["sha256"],
               "treeManifestSHA256": hashlib.sha256(canonical).hexdigest(),
               "compilerRoot": str(args.directory), "fileCount": len(entries),
               "fileBytes": total, "files": entries, "toolExecuted": False,
               "wholeFirmwareSourceComplete": False}
    with args.output.open("x") as out:
        json.dump(receipt, out, indent=2)
        out.write("\n")
    print(json.dumps({key: value for key, value in receipt.items() if key != "files"}))


if __name__ == "__main__":
    main()
