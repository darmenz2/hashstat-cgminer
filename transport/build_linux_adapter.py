#!/usr/bin/env python3

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from build_cgminer_core import host_identity, default_toolchain_lock
TREE_SHA = "0df8ff6b8ad4f2969ed79cb22ab2934438705da4a00ff8b7f65abe4fcd3a06ac"
NAMES = ["transport/include/hs_aml_uart.h", "transport/include/hs_aml_uart_linux.h",
         "transport/src/aml_uart.c", "transport/runtime/aml_uart_linux.c",
         "transport/tests/test_aml_uart.c", "transport/tests/test_aml_uart_linux.c",
         "transport/tests/verify_linux_uapi.c", "transport/tests/uart_linux_linkcheck.c",
         "transport/build_linux_adapter.py"]


NAMES += ["include/hs_span.h"]

NAMES += [name for name in ("tools/build_cgminer_core.py", "tools/sync_core_portable.py",
                            "tools/verify_zig_toolchain.py") if name not in NAMES]


def digest(path):
    if path.is_symlink() or not path.is_file():
        raise ValueError("regular file required: " + str(path))
    return hashlib.sha256(path.read_bytes()).hexdigest()


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
    out = Path(tempfile.mkdtemp(prefix="hashstat-uart-linux-", dir=args.output_parent))
    receipt = {"schema": "hashstat.aml.uart-linux-adapter-build.v1", "sourceSHA256": before,
               "outputDirectory": str(out), "commands": [], "success": False,
               "armExecuted": False, "hardwareAccessed": False, "installed": False,
               "productionIntegration": False, "zigExecutableSHA256": zig_sha}
    env = {"PATH": os.environ.get("PATH", os.defpath), "LC_ALL": "C",
           "HOME": os.environ["HOME"], "TMPDIR": str(out),
           "ZIG_GLOBAL_CACHE_DIR": str(out / "zig-cache"),
           "ASAN_OPTIONS": "detect_leaks=" + ("0" if args.host.endswith("-macos") else "1") + ":halt_on_error=1", "UBSAN_OPTIONS": "halt_on_error=1"}

    def run(name, args, timeout=180):
        record = {"name": name, "argv": args, "cwd": str(out), "returncode": None}
        receipt["commands"].append(record)
        log = out / (name + ".log")
        record["log"] = str(log)
        print("BUILD_STAGE", name, flush=True)
        with log.open("xb") as stream:
            result = subprocess.run(args, cwd=out, env=env, stdout=stream,
                                    stderr=subprocess.STDOUT, timeout=timeout)
        record["returncode"] = result.returncode
        if result.returncode:
            raise RuntimeError(name + " failed: " + str(log))

    try:
        receipt["host"] = args.host
        receipt["zigExecutableSHA256"] = zig_sha
        receipt["toolchainLockSHA256"] = lock_sha
        receipt["nativeCompiler"] = {"path": args.cc, "sha256": digest(Path(args.cc))}
        receipt["readelf"] = {"path": args.readelf, "sha256": digest(Path(args.readelf))}
        verifier = ROOT / "tools/verify_zig_toolchain.py"
        run("toolchain", [sys.executable, str(verifier), "--archive",
            str(args.zig_archive),
            "--directory", str(args.zig.parent), "--output", str(out / "toolchain.json"), "--lock", str(args.toolchain_lock)])
        tool = json.loads((out / "toolchain.json").read_text())
        if (tool["status"] != "PASSED" or tool["lockSHA256"] != lock_sha or
                (args.host == "aarch64-macos" and tool["treeManifestSHA256"] != TREE_SHA)):
            raise ValueError("compiler tree mismatch")
        receipt["toolchain"] = {key: tool[key] for key in
                               ("archiveSHA256", "lockSHA256", "treeManifestSHA256", "fileCount")}
        receipt["toolchainVerifierSHA256"] = digest(verifier)
        snapshot = out / "source"
        for name in NAMES:
            dest = snapshot / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            with (ROOT / name).open("rb") as source, dest.open("xb") as target:
                shutil.copyfileobj(source, target)
            if digest(dest) != before[name]:
                raise ValueError("snapshot changed: " + name)
        flags = ["-std=c11", "-O2", "-UNDEBUG", "-Wall", "-Wextra", "-Wconversion", "-Werror", "-pedantic",
                 "-I", str(snapshot / "include"), "-I", str(snapshot / "transport/include")]
        sanitize = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        sources = [str(snapshot / name) for name in NAMES[2:4]]
        for label, test, extra in [("core", NAMES[4], sources[:1]), ("adapter", NAMES[5], sources)]:
            binary = out / ("test-" + label)
            run(label + "-compile", [args.cc] + flags + sanitize +
                [str(snapshot / test)] + extra + ["-o", str(binary)])
            run(label + "-mock", [str(binary)])
        arm = [str(args.zig), "cc", "-target", "arm-linux-musleabihf", "-mcpu=cortex_a9",
               "-mfpu=vfpv3-d16", "-mfloat-abi=hard"] + flags
        run("arm-uapi", arm + ["-c", str(snapshot / NAMES[6]), "-o", str(out / "uart-uapi.o")])
        binary = out / "uart-linux-linkcheck.arm"
        run("arm-static-link", arm + ["-static",
            "-ffile-prefix-map=" + str(snapshot) + "=/usr/src/hashstat/uart-linux",
            str(snapshot / NAMES[7])] + sources + ["-o", str(binary)])
        run("elf-inspection", [args.readelf, "-h", "-l", "-d", "-A", "-s", str(binary)])
        blob = binary.read_bytes()
        if blob[:6] != b"\x7fELF\x01\x01" or int.from_bytes(blob[18:20], "little") != 40:
            raise ValueError("unexpected ARM ELF")
        phoff = int.from_bytes(blob[28:32], "little")
        phsize = int.from_bytes(blob[42:44], "little")
        phcount = int.from_bytes(blob[44:46], "little")
        if phsize != 32 or phoff + phsize * phcount > len(blob):
            raise ValueError("invalid program header table")
        types = [int.from_bytes(blob[phoff + i * phsize:phoff + i * phsize + 4], "little")
                 for i in range(phcount)]
        if 2 in types or 3 in types:
            raise ValueError("ARM result is not standalone static ELF")
        elf = (out / "elf-inspection.log").read_text()
        if "native_open" not in elf or "native_control" not in elf:
            raise ValueError("native adapter was not retained in ARM link")
        if before != {name: digest(ROOT / name) for name in NAMES}:
            raise ValueError("sources changed during build")
        receipt["binary"] = {"path": str(binary), "size": len(blob), "sha256": digest(binary),
                             "elfClass": 32, "endianness": "little", "machine": "ARM",
                             "staticNoInterpreterOrDynamicSegment": True}
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
