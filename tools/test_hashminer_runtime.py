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


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=None)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    compiler = shutil.which(args.cc)
    if compiler is None:
        parser.error("native compiler unavailable")
    root = Path(__file__).resolve().parents[1]
    stage = Path(tempfile.mkdtemp(prefix="hashstat-hashminer-runtime-"))
    stage.chmod(0o700)
    names = [
        "runtime/hashminer_runtime.c", "runtime/hashminer_runtime_owner.c",
        "runtime/miner_owner.c", "runtime/aml_readonly.c",
        "src/miner_lifecycle.c", "src/aml_observe.c", "src/aml_presence.c",
        "src/aml88_profile.c", "src/work.c", "src/pow.c", "src/job_cache.c",
        "src/bm1362_rx.c", "src/btm_work_wire.c",
        "transport/runtime/aml_chain.c", "transport/src/aml_uart.c",
        "transport/src/bm1362_integrity.c",
        "core/hashstat-aml88-bridge.c", "tests/test_hashminer_runtime.c",
    ]
    headers = ["include/hs_hashminer_runtime.h", "core/hashstat-aml88-bridge.h"]
    headers += [str(p.relative_to(root)) for p in (root / "include").glob("*.h")]
    headers += [str(p.relative_to(root)) for p in (root / "transport/include").glob("*.h")]
    inputs = sorted(set(names + headers + [str(Path(__file__).resolve().relative_to(root))]))
    before = {name: digest(root / name) for name in inputs}
    receipt: dict[str, object] = {"scope": "native real local children, synthetic attributes; no device/network/package",
                                "stage": str(stage), "source_before": before, "commands": [], "passed": False}
    common = [compiler, "-std=c11", "-UNDEBUG", "-Wall", "-Wextra", "-Wconversion", "-Werror", "-pedantic"]
    for include in ("include", "transport/include", "core"):
        common += ["-I", str(root / include)]

    def run(name: str, argv: list[str], expected_failure: bool = False) -> str:
        env = os.environ.copy()
        leaks = "0" if sys.platform == "darwin" else "1"
        receipt["leak_sanitizer"] = "unsupported on macOS; not tested" if leaks == "0" else "enabled"
        env["ASAN_OPTIONS"] = f"detect_leaks={leaks}:halt_on_error=1"
        env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        result = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, check=False, timeout=45, env=env)
        (stage / f"{name}.log").write_text(result.stdout)
        receipt["commands"].append({"name": name, "argv": argv, "exit": result.returncode})
        if (expected_failure and result.returncode == 0) or (not expected_failure and result.returncode != 0):
            raise RuntimeError(f"{name} exited {result.returncode}; logs: {stage}")
        return result.stdout

    try:
        receipt["compiler"] = run("compiler-version", [compiler, "--version"])
        for name, flags in (("native", ["-O2"]),
                            ("sanitized", ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"])):
            binary = stage / f"test-runtime-{name}"
            run(name + "-compile", common + flags + [str(root / n) for n in names] + ["-o", str(binary)])
            output = run(name + "-run", [str(binary)])
            match = re.fullmatch(r"hashminer runtime: ([0-9]+) parent checks PASS \(real local children; synthetic attributes; no hardware\)\n", output)
            if match is None:
                raise RuntimeError(f"{name} missing exact clean completion marker")
            receipt[name] = {"parent_checks": int(match.group(1)), "sha256": digest(binary), "path": str(binary)}
            print(output.strip())
        negative = run("ndebug-rejected", common + ["-DNDEBUG", "-fsyntax-only", str(root / "tests/test_hashminer_runtime.c")], True)
        if "Runtime ownership tests must not define NDEBUG" not in negative:
            raise RuntimeError("negative compile failed for an unexpected reason")
        after = {name: digest(root / name) for name in inputs}
        receipt["source_after"] = after
        receipt["source_unchanged"] = before == after
        if before != after:
            raise RuntimeError("sources changed during test build/run")
        receipt["passed"] = True
    finally:
        receipt_path = stage / "receipt.json"
        receipt_path.write_text(json.dumps(receipt, indent=2) + "\n")
        receipt_path.chmod(0o600)
        print(f"receipt: {receipt_path}")


if __name__ == "__main__":
    main()
