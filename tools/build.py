#!/usr/bin/env python3

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys


NATIVE_SANITIZER_FLAGS = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                          "-fno-omit-frame-pointer"]


def native_sanitizer_environment():
    env = os.environ.copy()

    leaks = "0" if sys.platform == "darwin" else "1"
    env["ASAN_OPTIONS"] = "detect_leaks=" + leaks + ":halt_on_error=1"
    env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    return env


def digest(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=None)
    ap.add_argument("--output", type=Path, required=True)
    ap.add_argument("--cc", default="clang")
    ap.add_argument("--arm-ld", default="arm-none-eabi-ld")
    args = ap.parse_args(); root = Path(__file__).resolve().parent.parent; out = args.output
    if not out.is_absolute() or out.exists() or out.is_symlink() or not out.parent.is_dir():
        raise ValueError("output must be a new absolute directory under an existing parent")
    cc = shutil.which(args.cc); ld = shutil.which(args.arm_ld)
    if not cc or not ld:
        raise ValueError("clang and arm-none-eabi-ld are required")
    modules = ["measurement", "chip_frequencies", "aml_frame", "pow", "work",
               "btm_work_wire", "bm1362_rx", "job_cache", "bm1362_commands", "bm1362_pll",
               "miner_lifecycle", "aml_presence", "aml_observe", "aml88_profile", "bm1362_setup"]
    test_links = {"work": ["pow"], "job_cache": ["work", "pow", "bm1362_rx"],
                  "btm_header_bridge": ["btm_work_wire"],
                  "aml_observe": ["aml_presence"], "aml_readonly": ["aml_presence"],
                  "bm1362_setup": ["bm1362_commands", "aml_frame"],
                  "captured_work": ["bm1362_rx", "job_cache", "work", "pow", "btm_work_wire"]}
    names = [kind + "/" + prefix + module + suffix for module in modules
             for kind, prefix, suffix in [("src", "", ".c"), ("include", "hs_", ".h"), ("tests", "test_", ".c")]]
    names += ["tests/arm_probe.c", "tests/freestanding_runtime.c", "tools/build.py", "tools/run_arm_probe.py", "tools/differential_work.py", "tools/differential_pow.py"]
    names += ["runtime/linux_selftest.c", "runtime/linux_start.S"]
    names += ["tests/test_btm_header_bridge.c"]
    names += ["tests/test_captured_work.c"]
    names += ["tests/fixtures/captured-work.json", "tools/differential_captured_work.py"]
    names += ["runtime/aml_readonly.c", "include/hs_aml_readonly.h", "tests/test_aml_readonly.c"]
    names += ["include/hs_span.h", "tests/test_span.c", "tools/test_span.py"]
    for name in names:
        if not stat.S_ISREG((root / name).lstat().st_mode): raise ValueError("source must be regular: " + name)
    sources = {name: digest(root / name) for name in names}
    out.mkdir(mode=0o700)
    records = []

    def run(command, limit=120, env=None):
        index = len(records)
        try:
            result = subprocess.run(command, cwd=root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=limit, text=True, env=env)
        except subprocess.TimeoutExpired as error:
            partial = error.stdout or ""
            if isinstance(partial, bytes): partial = partial.decode("utf-8", "replace")
            (out / ("command-%02d.log" % index)).write_text(partial)
            records.append({"argv": list(map(str, command)), "returncode": None, "timedOutSeconds": limit, "log": "command-%02d.log" % index})
            raise
        (out / ("command-%02d.log" % index)).write_text(result.stdout)
        records.append({"argv": list(map(str, command)), "returncode": result.returncode, "log": "command-%02d.log" % index})
        if result.returncode: raise RuntimeError("command failed: " + str(command) + "\n" + result.stdout)
        return result.stdout

    common = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Wconversion", "-Werror", "-pedantic", "-fno-fast-math", "-ffp-contract=off", "-I", str(root / "include")]
    sanitizer_env = native_sanitizer_environment()
    sanitizer_options = {name: sanitizer_env[name] for name in ("ASAN_OPTIONS", "UBSAN_OPTIONS")}
    try:
        versions = {"cc": run([cc, "--version"]).splitlines()[0], "armLinker": run([ld, "--version"]).splitlines()[0]}
        for name in modules + ["span", "btm_header_bridge", "aml_readonly", "captured_work"]:
            binary = out / ("test_" + name)
            dependencies = ([name] if name in modules else []) + test_links.get(name, [])
            inputs = [str(root / "src" / (dependency + ".c")) for dependency in dependencies]
            run([cc] + common + NATIVE_SANITIZER_FLAGS + inputs + [str(root / "tests" / ("test_" + name + ".c")), "-o", str(binary)])
            print(run([str(binary)], env=sanitizer_env).strip())
        library = out / ("libhs-reconstruction.dylib" if sys.platform == "darwin" else "libhs-reconstruction.so")
        run([cc] + common + (["-dynamiclib"] if sys.platform == "darwin" else ["-shared", "-fPIC"]) + [str(root / "src" / (module + ".c")) for module in modules] + [str(root / "tests/arm_probe.c"), "-o", str(library)])
        arm = ["--target=armv7a-linux-gnueabihf", "-march=armv7-a", "-marm", "-mfpu=vfpv3-d16", "-mfloat-abi=hard", "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-fno-unwind-tables", "-fno-asynchronous-unwind-tables", "-fno-vectorize", "-fno-slp-vectorize"]
        objects = []
        for name in ["src/" + module + ".c" for module in modules] + ["tests/arm_probe.c", "tests/freestanding_runtime.c"]:
            obj = out / (Path(name).stem + ".arm.o"); objects.append(str(obj))
            run([cc] + common + arm + ["-c", str(root / name), "-o", str(obj)])
        probe = out / "hs-reconstruction-probe.arm.elf"


        run([ld, "-m", "armelf", "-e", "hs_arm_probe", "-Ttext=0x10000", "--build-id=none", "--no-undefined", "--fatal-warnings", "-o", str(probe)] + objects)
        runtime_objects = []
        for name in ["runtime/linux_selftest.c", "runtime/linux_start.S"]:
            obj = out / (Path(name).stem + ".arm.o"); runtime_objects.append(str(obj))
            flags = common if name.endswith(".c") else ["-I", str(root / "include")]
            run([cc] + flags + arm + ["-c", str(root / name), "-o", str(obj)])
        linux_test = out / "hashstat-aml88-selftest.arm"
        run([ld, "-m", "armelf", "-e", "_start", "-Ttext=0x10000", "--build-id=none", "--no-undefined", "--fatal-warnings", "-o", str(linux_test)] + objects + runtime_objects)
        for name, sha in sources.items():
            if digest(root / name) != sha: raise ValueError("source changed during build: " + name)
        outputs = {p.name: digest(p) for p in out.iterdir() if p.is_file() and not p.name.endswith(".log")}
        receipt = {"status": "MODULE_BUILD_AND_NATIVE_TESTS_PASSED", "sourceSHA256": sources, "outputSHA256": outputs, "versions": versions, "commands": records, "nativeSanitizers": ["address", "undefined"], "armTarget": "ARMv7-A VFPv3-D16 EABI hard-float", "armExecutionVerified": False, "hardwareTestPerformed": False, "completeCgminer": False, "signed": False, "deployableFirmware": False}
        receipt["nativeSanitizerOptions"] = sanitizer_options
        receipt["leakSanitizer"] = "unsupported on macOS; not tested" if sys.platform == "darwin" else "enabled"
        (out / "build-receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
        print(json.dumps({"status": receipt["status"], "output": str(out), "armProbeSHA256": digest(probe), "completeCgminer": False}))
    except Exception:
        (out / "build-failure.json").write_text(json.dumps({"status": "FAILED", "commands": records, "nativeSanitizerOptions": sanitizer_options, "completeCgminer": False}, indent=2) + "\n")
        raise


if __name__ == "__main__":
    main()
