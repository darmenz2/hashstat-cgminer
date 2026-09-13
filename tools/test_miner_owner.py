#!/usr/bin/env python3

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=None)
    parser.add_argument("--cc", default="cc", help="Local native C compiler (default: cc)")
    parser.add_argument("--arm-compile", action="store_true",
                        help="Also compile two freestanding ARM objects with /usr/bin/clang; do not execute")
    args = parser.parse_args()
    compiler = shutil.which(args.cc)
    if compiler is None:
        parser.error("local C compiler not found")
    root = Path(__file__).resolve().parents[1]
    inputs = [
        root / "include/hs_span.h",
        root / "include/hs_miner_owner.h",
        root / "include/hs_miner_lifecycle.h",
        root / "runtime/miner_owner.c",
        root / "src/miner_lifecycle.c",
        root / "tests/test_miner_owner.c",
        Path(__file__).resolve(),
    ]
    before = {str(p.relative_to(root)): sha256(p) for p in inputs}
    stage = Path(tempfile.mkdtemp(prefix="hashstat-miner-owner-"))
    stage.chmod(0o700)
    receipt: dict[str, object] = {
        "scope": "native injected callbacks; no device, network or firmware package",
        "source_before": before,
        "stage": str(stage),
        "commands": [],
        "passed": False,
    }
    common = [compiler, "-std=c11", "-UNDEBUG", "-Wall", "-Wextra", "-Wconversion",
              "-Werror", "-pedantic", "-I", str(root / "include")]
    sources = [str(root / p) for p in
               ("runtime/miner_owner.c", "src/miner_lifecycle.c", "tests/test_miner_owner.c")]

    def run(name: str, command: list[str], *, expected: int = 0,
            env: dict[str, str] | None = None) -> str:
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, timeout=60, check=False, env=env)
        (stage / (name + ".log")).write_text(result.stdout)
        receipt["commands"].append({"name": name, "argv": command, "exit": result.returncode})
        if (expected == 0 and result.returncode != 0) or (expected != 0 and result.returncode == 0):
            raise RuntimeError(f"{name}: unexpected exit {result.returncode}; see {stage}")
        return result.stdout

    try:
        receipt["compiler"] = run("compiler-version", [compiler, "--version"])
        checks = []
        for name, flags in (("native", ["-O2"]),
                            ("sanitized", ["-O1", "-g", "-fsanitize=address,undefined",
                                           "-fno-omit-frame-pointer"])):
            binary = stage / ("test-miner-owner-" + name)
            run(name + "-compile", common + flags + sources + ["-o", str(binary)])
            test_env = os.environ.copy()

            leaks = "0" if sys.platform == "darwin" else "1"
            receipt["leak_sanitizer"] = "unsupported on macOS; not tested" if leaks == "0" else "enabled"
            test_env["ASAN_OPTIONS"] = f"detect_leaks={leaks}:halt_on_error=1"
            test_env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
            output = run(name + "-run", [str(binary)], env=test_env)
            match = re.fullmatch(r"miner owner: ([0-9]+) checks PASS \(injected callbacks; no hardware\)\n", output)
            if match is None:
                raise RuntimeError(f"{name}: missing exact test completion marker")
            checks.append(int(match.group(1)))
            receipt[name] = {"checks": checks[-1], "binary": str(binary), "sha256": sha256(binary)}
        if checks[0] != checks[1]:
            raise RuntimeError("native and sanitizer test counts differ")
        negative = run("ndebug-rejected", common + ["-DNDEBUG", "-fsyntax-only", sources[2]], expected=1)
        if "Owner fault tests must not be compiled with NDEBUG" not in negative:
            raise RuntimeError("negative compile did not fail for the intended assertion guard")
        if args.arm_compile:
            arm_flags = ["/usr/bin/clang", "--target=armv7a-linux-gnueabihf", "-march=armv7-a",
                         "-marm", "-mfpu=vfpv3-d16", "-mfloat-abi=hard", "-ffreestanding",
                         "-fno-builtin", "-std=c11", "-Wall", "-Wextra", "-Wconversion",
                         "-Werror", "-pedantic", "-I", str(root / "include")]
            objects = []
            for number, source in enumerate(sources[:2]):
                obj = stage / f"arm-freestanding-{number}.o"
                run(f"arm-compile-{number}", arm_flags + ["-c", source, "-o", str(obj)])
                data = obj.read_bytes()
                if (len(data) < 52 or data[:7] != b"\x7fELF\x01\x01\x01" or
                        int.from_bytes(data[16:18], "little") != 1 or
                        int.from_bytes(data[18:20], "little") != 40):
                    raise RuntimeError("ARM compile did not produce an ELF32 little-endian ARM relocatable object")
                objects.append({"path": str(obj), "sha256": sha256(obj), "size": len(data)})
            receipt["arm_compile_only"] = {"executed": False, "objects": objects}
        after = {str(p.relative_to(root)): sha256(p) for p in inputs}
        receipt["source_after"] = after
        receipt["source_unchanged"] = before == after
        if before != after:
            raise RuntimeError("source changed while tests were running")
        receipt["passed"] = True
        print(f"miner owner: {checks[0]} native + {checks[1]} ASan/UBSan checks PASS")
        if args.arm_compile:
            print("ARM: 2 freestanding ELF32 objects compiled; not linked or executed")
    finally:
        receipt_path = stage / "receipt.json"
        receipt_path.write_text(json.dumps(receipt, indent=2) + "\n")
        receipt_path.chmod(0o600)
        print(f"receipt: {receipt_path}")


if __name__ == "__main__":
    main()
