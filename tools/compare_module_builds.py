#!/usr/bin/env python3

import argparse
import hashlib
import json
from pathlib import Path


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_build(path):
    require(path.is_absolute() and path.is_dir() and not path.is_symlink(), "absolute regular build directory required")
    receipt_path = path / "build-receipt.json"
    receipt = json.loads(receipt_path.read_text())
    require(receipt["status"] == "MODULE_BUILD_AND_NATIVE_TESTS_PASSED", "build not successful")
    require(all(row["returncode"] == 0 for row in receipt["commands"]), "failed recorded command")
    for name, expected in receipt["outputSHA256"].items():
        require(Path(name).name == name and name not in ("", ".", ".."), "unsafe output name")
        item = path / name
        require(item.is_file() and not item.is_symlink(), "regular output required")
        require(sha(item) == expected, "output differs from receipt: " + name)
    return receipt, sha(receipt_path)


def main():
    parser = argparse.ArgumentParser(description=None)
    parser.add_argument("--first", type=Path, required=True)
    parser.add_argument("--second", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    require(args.first.resolve() != args.second.resolve(), "two distinct builds required")
    require(args.output.is_absolute() and args.output.parent.is_dir() and
            not args.output.exists() and not args.output.is_symlink(), "new absolute output required")
    first, first_sha = read_build(args.first)
    second, second_sha = read_build(args.second)
    require(first["sourceSHA256"] == second["sourceSHA256"], "source sets differ")
    require(first["versions"] == second["versions"], "compiler versions differ")
    names = sorted(n for n in first["outputSHA256"] if n.endswith((".arm.o", ".arm.elf", "selftest.arm")))
    require(names and names == sorted(n for n in second["outputSHA256"] if n.endswith((".arm.o", ".arm.elf", "selftest.arm"))), "ARM output sets differ")
    same = all(first["outputSHA256"][n] == second["outputSHA256"][n] for n in names)
    report = {"status": "PASS" if same else "MISMATCH", "firstReceiptSHA256": first_sha,
              "secondReceiptSHA256": second_sha, "sourceCount": len(first["sourceSHA256"]),
              "sourceSetsIdentical": True, "compilerVersions": first["versions"],
              "armOutputs": {n: {"first": first["outputSHA256"][n], "second": second["outputSHA256"][n]} for n in names},
              "nativeOutputReproducibilityChecked": False, "hardwareAcceptance": False,
              "completeCgminer": False, "installReady": False,
              "toolSHA256": sha(Path(__file__).resolve())}
    with args.output.open("x") as handle:
        handle.write(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"status": report["status"], "sourceCount": report["sourceCount"],
                      "identicalARMOutputs": len(names) if same else 0, "installReady": False}))
    require(same, "ARM objects/executables differ")


if __name__ == "__main__":
    main()
