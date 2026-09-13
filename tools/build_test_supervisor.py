#!/usr/bin/env python3

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from build_cgminer_core import host_identity, default_toolchain_lock
TREE_SHA = "0df8ff6b8ad4f2969ed79cb22ab2934438705da4a00ff8b7f65abe4fcd3a06ac"
NAMES = ["runtime/hs_test_supervisor.c", "tests/test_test_supervisor_child.c",
         "tests/test_test_supervisor.py", "tools/build_test_supervisor.py",
         "tools/verify_zig_toolchain.py", "tools/build_cgminer_core.py",
         "tools/sync_core_portable.py", "tests/test_test_supervisor_wait.c"]


NAMES += [name for name in ("tools/build_cgminer_core.py", "tools/sync_core_portable.py",
                            "tools/verify_zig_toolchain.py") if name not in NAMES]


def digest(path):
    if path.is_symlink() or not path.is_file():
        raise ValueError("regular file required: " + str(path))
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inspect_arm(path):
    data = path.read_bytes()
    if len(data) < 52 or data[:7] != b"\x7fELF\x01\x01\x01":
        raise ValueError("expected little-endian ELF32")
    values = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    kind, machine, version, _, phoff, _, flags, ehsize, phsize, phnum, _, _, _ = values
    if kind != 2 or machine != 40 or version != 1 or ehsize != 52 or phsize != 32 or not phnum:
        raise ValueError("expected executable ARM ELF header")
    if not flags & 0x400 or flags & 0x200 or (flags >> 24) != 5:
        raise ValueError("expected ARM EABI5 hard-float ABI")
    if phoff + phnum * phsize > len(data):
        raise ValueError("invalid ELF program table")
    program_types = [struct.unpack_from("<I", data, phoff + index * phsize)[0] for index in range(phnum)]
    if 2 in program_types or 3 in program_types or 1 not in program_types:
        raise ValueError("expected static ELF without PT_DYNAMIC or PT_INTERP")
    return {"elfClass": 32, "endianness": "little", "machine": "ARM", "eabi": 5,
            "hardFloat": True, "static": True, "interpreter": False, "dynamicSegment": False}


def parse_options(argv=None):
    parser = argparse.ArgumentParser(description=None)
    parser.add_argument("--zig", type=Path, required=True)
    parser.add_argument("--zig-archive", type=Path, required=True)
    parser.add_argument("--toolchain-lock", type=Path)
    parser.add_argument("--output-parent", type=Path)
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--readelf", default="arm-none-eabi-readelf")
    args = parser.parse_args(argv)
    args.host = host_identity()
    args.zig = args.zig.resolve(strict=True)
    args.zig_archive = args.zig_archive.resolve(strict=True)
    args.toolchain_lock = (args.toolchain_lock or default_toolchain_lock(args.host)).resolve(strict=True)
    lock = json.loads(args.toolchain_lock.read_text())
    if lock.get("host") != args.host or lock.get("version") != "0.16.0":
        raise ValueError("locked compiler host/version differs from build host")
    args.output_parent = (args.output_parent or Path(tempfile.gettempdir())).resolve(strict=True)
    if not args.output_parent.is_dir():
        raise ValueError("existing output parent directory required")
    args.cc = shutil.which(args.cc)
    args.readelf = shutil.which(args.readelf)
    if not args.cc or not args.readelf:
        raise ValueError("native compiler and ARM-capable readelf are required")
    args.cc = str(Path(args.cc).resolve(strict=True))
    args.readelf = str(Path(args.readelf).resolve(strict=True))
    return args

def main():
    args = parse_options()
    zig_sha = digest(args.zig)
    lock_sha = digest(args.toolchain_lock)
    os.umask(0o077)
    before = {name: digest(ROOT / name) for name in NAMES}
    out = Path(tempfile.mkdtemp(prefix="hashstat-test-supervisor-", dir=args.output_parent))
    receipt = {"schema": "hashstat.test-supervisor-build.v1", "sourceSHA256": before,
               "outputDirectory": str(out), "commands": [], "success": False,
               "armExecuted": False, "linuxParentDeathExecuted": False,
               "hardwareTest": False, "networkAccess": False, "installed": False,
               "zigExecutableSHA256": zig_sha}
    env = {"PATH": os.environ.get("PATH", os.defpath), "LC_ALL": "C",
           "HOME": os.environ["HOME"], "TMPDIR": str(out),
           "ZIG_GLOBAL_CACHE_DIR": str(out / "zig-cache"),
           "ASAN_OPTIONS": "detect_leaks=" + ("0" if args.host.endswith("-macos") else "1") + ":halt_on_error=1", "UBSAN_OPTIONS": "halt_on_error=1"}

    def run(name, args, timeout=180):
        log = out / (name + ".log")
        record = {"name": name, "argv": args, "cwd": str(out), "returncode": None, "log": str(log)}
        receipt["commands"].append(record)
        print("BUILD_STAGE", name, flush=True)
        try:
            with log.open("xb") as stream:
                result = subprocess.run(args, cwd=out, env=env, stdout=stream,
                                        stderr=subprocess.STDOUT, timeout=timeout)
            record["returncode"] = result.returncode
        except subprocess.TimeoutExpired:
            record["timedOut"] = True
            raise
        if result.returncode:
            raise RuntimeError(name + " failed: " + str(log))

    try:
        receipt["host"] = args.host
        receipt["zigExecutableSHA256"] = zig_sha
        receipt["toolchainLockSHA256"] = lock_sha
        receipt["nativeCompiler"] = {"path": args.cc, "sha256": digest(Path(args.cc))}
        receipt["readelf"] = {"path": args.readelf, "sha256": digest(Path(args.readelf))}
        verifier = ROOT / NAMES[4]
        run("toolchain", [sys.executable, str(verifier), "--archive",
                          str(args.zig_archive),
                          "--directory", str(args.zig.parent), "--output", str(out / "toolchain.json"), "--lock", str(args.toolchain_lock)])
        tool = json.loads((out / "toolchain.json").read_text())
        if (tool["status"] != "PASSED" or tool["lockSHA256"] != lock_sha or
                (args.host == "aarch64-macos" and tool["treeManifestSHA256"] != TREE_SHA)):
            raise ValueError("compiler tree mismatch")
        receipt["toolchain"] = {key: tool[key] for key in
                               ("archiveSHA256", "lockSHA256", "treeManifestSHA256", "fileCount")}
        snapshot = out / "source"
        for name in NAMES:
            destination = snapshot / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            with (ROOT / name).open("rb") as source, destination.open("xb") as target:
                shutil.copyfileobj(source, target)
            if digest(destination) != before[name]:
                raise ValueError("source snapshot changed: " + name)
        flags = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Wconversion", "-Werror", "-pedantic"]
        sanitize = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        run("native-wait-probe-compile", [args.cc] + flags +
            [str(snapshot / NAMES[7]), "-o", str(out / "test-supervisor-wait")])
        run("native-wait-probe", [str(out / "test-supervisor-wait")])
        for flavor, extra in (("native", []), ("sanitized", sanitize)):
            supervisor = out / ("hashstat-test-supervisor." + flavor)
            fixture = out / ("test-supervisor-child." + flavor)
            run(flavor + "-compile", [args.cc] + flags + extra +
                [str(snapshot / NAMES[0]), "-o", str(supervisor)])
            run(flavor + "-fixture-compile", [args.cc] + flags + extra +
                [str(snapshot / NAMES[1]), "-o", str(fixture)])
            report = out / (flavor + "-tests.json")
            run(flavor + "-tests", [sys.executable, str(snapshot / NAMES[2]),
                "--supervisor", str(supervisor), "--child", str(fixture), "--receipt", str(report)])
            results = json.loads(report.read_text())
            if not results["success"]:
                raise ValueError("native fixture suite incomplete")
            receipt[flavor + "Tests"] = {"receipt": str(report), "sha256": digest(report),
                                        "passed": results["passed"], "skipped": results["skipped"]}
        binary = out / "hashstat-test-supervisor.arm"
        run("arm-compile", [str(args.zig), "cc", "-target", "arm-linux-musleabihf",
            "-mcpu=cortex_a9", "-mfpu=vfpv3-d16", "-mfloat-abi=hard", "-static"] + flags +
            ["-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections", "-Wl,--strip-debug",
             "-ffile-prefix-map=" + str(snapshot) + "=/usr/src/hashstat/test-supervisor",
             str(snapshot / NAMES[0]), "-o", str(binary)])
        run("elf-inspection", [args.readelf, "-h", "-l", "-d", "-A", str(binary)])
        receipt["elf"] = inspect_arm(binary)
        if before != {name: digest(ROOT / name) for name in NAMES}:
            raise ValueError("sources changed during build")
        receipt["binary"] = {"path": str(binary), "size": binary.stat().st_size, "sha256": digest(binary)}
        if digest(args.zig) != zig_sha or digest(args.toolchain_lock) != lock_sha:
            raise ValueError("compiler executable or lock changed during build")
        receipt["success"] = True
        return 0
    finally:
        with (out / "receipt.json").open("x") as stream:
            json.dump(receipt, stream, indent=2)
            stream.write("\n")
        print(json.dumps({"directory": str(out), "success": receipt["success"],
                          "binary": receipt.get("binary"), "armExecuted": False}), flush=True)


if __name__ == "__main__":
    raise SystemExit(main())
