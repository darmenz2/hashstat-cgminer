#!/usr/bin/env python3

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time

from sync_core_portable import PORTABLE

PIN = "b8491c66e7e22f23a9edf095dd1337ee581e88bd"
ZIG_TREE_PIN = "0df8ff6b8ad4f2969ed79cb22ab2934438705da4a00ff8b7f65abe4fcd3a06ac"
ROOT = Path(__file__).resolve().parent.parent


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def metadata(paths: list[str], root: Path) -> list[dict]:
    result = []
    for name in paths:
        rel = Path(name)
        if (rel.is_absolute() or ".." in rel.parts or not rel.parts or name != rel.as_posix()
                or "\\" in name or any(ord(char) < 32 for char in name) or ".git" in rel.parts):
            raise ValueError("unsafe source path")
        current = root
        for component in rel.parts:
            current /= component
            if current.is_symlink():
                raise ValueError(f"source symlink forbidden: {name}")
        if not current.is_file():
            raise ValueError(f"nonregular source: {name}")
        result.append({"path": name, "size": current.stat().st_size,
                       "executable": bool(current.stat().st_mode & 0o111),
                       "sha256": digest(current)})
    return result


def source_names(source: Path) -> list[str]:
    names = sorted(set(subprocess.check_output(
        ["git", "-C", str(source), "ls-files", "-z", "--cached", "--others", "--exclude-standard"]
    ).decode().split("\0")) - {""})
    return [name for name in names if ".DS_Store" not in Path(name).parts]


def source_inputs(source: Path, lock_path: Path | None) -> tuple[list[dict], str, dict | None]:

    if lock_path:
        lock = json.loads(lock_path.read_text())
        if lock.get("schema") != "hashstat.cgminer-core-source.v1" or lock.get("upstreamCommit") != PIN:
            raise ValueError("source lock schema/upstream pin mismatch")
        rows = lock["sourceFiles"]
        names = [row["path"] for row in rows]
        if names != sorted(set(names)) or not names:
            raise ValueError("source lock paths must be nonempty, unique and sorted")
        if metadata(names, source) != rows:
            raise ValueError("source differs from reviewed source lock")
        epoch = str(lock["sourceDateEpoch"])
        if not epoch.isdecimal():
            raise ValueError("invalid locked source epoch")
        return rows, epoch, lock
    head = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    if head != PIN:
        raise ValueError("upstream commit pin mismatch")
    epoch = subprocess.check_output(
        ["git", "-C", str(source), "show", "-s", "--format=%ct", "HEAD"], text=True).strip()
    return metadata(source_names(source), source), epoch, None


def verify_portable(source: Path, lock: dict | None) -> list[dict]:

    rows = [{"path": "hashstat-pure/" + Path(rel).name,
             "sha256": digest(source / "hashstat-pure" / Path(rel).name)} for rel in PORTABLE]
    expected = lock["portableFiles"] if lock else [
        {"path": "hashstat-pure/" + Path(rel).name, "sha256": digest(ROOT / rel)} for rel in PORTABLE]
    if rows != expected:
        raise ValueError("requested source portable mirror differs from reviewed inputs")
    return rows


def host_identity(system: str | None = None, machine: str | None = None) -> str:
    system = system or platform.system()
    machine = machine or platform.machine()
    arch = {"arm64": "aarch64", "aarch64": "aarch64", "amd64": "x86_64", "x86_64": "x86_64"}.get(machine.lower())
    if system not in ("Linux", "Darwin") or not arch:
        raise ValueError("supported hosts: Linux x86_64/aarch64, macOS aarch64")
    return arch + ("-linux" if system == "Linux" else "-macos")


def default_toolchain_lock(host: str) -> Path:
    if host == "aarch64-macos":
        return ROOT / "toolchains/zig-0.16.0.lock.json"
    return ROOT / ("toolchains/zig-0.16.0-" + host + ".lock.json")


def expected_version(source: Path) -> str:
    matches = re.findall(r'^#define HASHSTAT_VERSION "([^"\n]+)"$',
                         (source / "hashstat-version.h").read_text(), re.MULTILINE)
    if len(matches) != 1:
        raise ValueError("source HASHSTAT_VERSION is absent/ambiguous")
    return matches[0]


def native_compiler(requested: Path | None, search_path: str) -> str:
    if requested:
        if not requested.is_absolute():
            raise ValueError("--native-cc must be an absolute compiler path")
        executable = requested.resolve(strict=True)
        if not executable.is_file() or not os.access(executable, os.X_OK):
            raise ValueError("--native-cc must be an executable regular compiler")
        return str(executable)
    executable = shutil.which("clang", path=search_path)
    if not executable:
        raise ValueError("native test prerequisite unavailable: clang (or pass --native-cc)")
    return executable


def native_test_environment(system: str) -> dict[str, str]:
    return {"CFLAGS": "-O1 -g -fno-strict-aliasing -fcommon -fsanitize=address,undefined -fno-sanitize-recover=all",
            "LDFLAGS": "-fsanitize=address,undefined",
            "ASAN_OPTIONS": "detect_leaks=" + ("1" if system == "Linux" else "0") + ":halt_on_error=1:abort_on_error=1",
            "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"}


def arm_elf_metadata(data: bytes) -> dict:

    if len(data) < 52 or data[:7] != b"\x7fELF\x01\x01\x01":
        raise ValueError("not a 32-bit little-endian ELF")
    eh = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    etype, machine, version, entry, phoff, shoff, flags, ehsize, phsize, phnum, shsize, shnum, shstridx = eh
    if (machine != 40 or etype != 2 or version != 1 or ehsize != 52 or
            (flags & 0xff000000) != 0x05000000 or not flags & 0x400 or flags & 0x200):
        raise ValueError("not ARM Linux EABI5 hard-float ET_EXEC")
    if phsize != 32 or phnum == 0 or phoff + phsize * phnum > len(data):
        raise ValueError("invalid ELF program table")
    programs = [struct.unpack_from("<IIIIIIII", data, phoff + phsize * index) for index in range(phnum)]
    if any(program[0] in (2, 3) for program in programs):
        raise ValueError("unexpected dynamic linkage/interpreter")
    if not any(program[0] == 1 for program in programs):
        raise ValueError("ELF has no loadable segment")
    for program in programs:
        if program[1] + program[4] > len(data) or (program[0] == 1 and program[4] > program[5]):
            raise ValueError("invalid ELF segment extent")
    if shsize != 40 or not 0 < shnum < 10000 or shstridx >= shnum or shoff + shsize * shnum > len(data):
        raise ValueError("invalid ELF section table")
    sections = [struct.unpack_from("<IIIIIIIIII", data, shoff + shsize * index) for index in range(shnum)]
    strings = sections[shstridx]
    if strings[1] != 3 or strings[4] + strings[5] > len(data):
        raise ValueError("invalid ELF section name table")
    names = data[strings[4]:strings[4] + strings[5]]
    allocated, debug, seen = [], [], set()
    for section in sections[1:]:
        nameoff, kind, attributes, address, offset, size, link, info, alignment, entry_size = section
        if nameoff >= len(names) or names.find(b"\0", nameoff) < 0:
            raise ValueError("invalid ELF section name")
        name = names[nameoff:names.index(b"\0", nameoff)].decode("ascii")
        if kind != 8 and offset + size > len(data):
            raise ValueError("invalid ELF section extent")
        if name.startswith((".debug", ".zdebug", ".stab")):
            debug.append(name)
        if attributes & 2:
            if name in seen:
                raise ValueError("duplicate allocated section")
            seen.add(name)
            allocated.append({"name": name, "type": kind, "flags": attributes, "address": address,
                              "size": size, "alignment": alignment, "entrySize": entry_size,
                              "sha256": None if kind == 8 else hashlib.sha256(data[offset:offset + size]).hexdigest()})
    if not allocated:
        raise ValueError("ELF has no allocated sections")
    return {"elfMachine": "ARM", "classBits": 32, "eabi": 5, "hardFloat": True,
            "staticProgramHeaders": True, "entry": entry, "elfFlags": flags,
            "programHeaders": [list(program) for program in programs],
            "allocatedSections": sorted(allocated, key=lambda row: row["name"]), "debugSections": sorted(debug)}


def validate_release_pair(diagnostic: bytes, release: bytes) -> tuple[dict, dict]:
    before, after = arm_elf_metadata(diagnostic), arm_elf_metadata(release)
    if after["debugSections"]:
        raise ValueError("release ELF still contains debug sections")


    for field in ("entry", "elfFlags", "programHeaders", "allocatedSections"):
        if before[field] != after[field]:
            raise ValueError("release transform changed allocated code/data or mapping: " + field)
    return before, after


def verified_objcopy(path: Path, expected_sha: str) -> Path:
    if not path.is_absolute() or not re.fullmatch(r"[0-9a-f]{64}", expected_sha):
        raise ValueError("objcopy requires an absolute executable path and lowercase SHA256 pin")
    path = path.resolve(strict=True)
    if not path.is_file() or not os.access(path, os.X_OK) or digest(path) != expected_sha:
        raise ValueError("objcopy executable pin/type mismatch")
    return path


def main() -> int:
    parser = argparse.ArgumentParser(description=None)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--zig", type=Path, help="cross-build compiler; not needed for native tests/source lock")
    parser.add_argument("--zig-sha256", help="optional additional executable digest; whole archive/tree is always verified")
    parser.add_argument("--zig-archive", type=Path, help="default: archive next to extracted compiler directory")
    parser.add_argument("--toolchain-lock", type=Path, help="default: bundled official lock for this host")
    parser.add_argument("--output-parent", type=Path, help="existing space-free staging parent; default: platform temp directory")
    locks = parser.add_mutually_exclusive_group()
    locks.add_argument("--source-lock", type=Path, help="require exact reviewed source; allows Git-free source export")
    locks.add_argument("--record-source-lock", type=Path, help="freeze reviewed Git source metadata only; do not build")
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--objcopy", type=Path,
                        help="explicit trusted ARM-capable GNU/LLVM objcopy for debug-free release ELF")
    parser.add_argument("--objcopy-sha256", help="required executable SHA256 pin when --objcopy is supplied")
    parser.add_argument("--native-tests", action="store_true",
                        help="ASan/UBSan native tests and early information-only main; never mining startup")
    parser.add_argument("--native-cc", type=Path,
                        help="explicit absolute native Clang executable, e.g. /usr/bin/clang-21; default: host clang")
    args = parser.parse_args()
    source = args.source.resolve(strict=True)
    before, source_epoch, source_lock = source_inputs(source, args.source_lock)
    names = [row["path"] for row in before]
    if not names or "driver-hashstat-aml88.c" not in names:
        raise ValueError("HashStat source integration absent")
    portable_verifier = Path(__file__).resolve().parent / "sync_core_portable.py"
    portable_rows = verify_portable(source, source_lock)
    if args.record_source_lock:
        if metadata(source_names(source), source) != before:
            raise ValueError("source changed while recording lock")
        locked = {"schema": "hashstat.cgminer-core-source.v1", "upstreamCommit": PIN,
                  "sourceDateEpoch": source_epoch, "sourceFiles": before, "portableFiles": portable_rows,
                  "scope": "HashMiner core source"}
        with args.record_source_lock.open("x") as out:
            json.dump(locked, out, indent=2, sort_keys=True)
            out.write("\n")
        print("SOURCE_LOCK", args.record_source_lock, "SHA256", digest(args.record_source_lock))
        return 0
    if args.jobs < 1 or args.jobs > 64:
        parser.error("--jobs must be between 1 and 64")
    host = host_identity()
    objcopy = None
    if not args.native_tests:
        if bool(args.objcopy) != bool(args.objcopy_sha256):
            parser.error("--objcopy and --objcopy-sha256 must be supplied together")
        if platform.system() == "Linux" and not args.objcopy:
            parser.error("Linux release builds require --objcopy and --objcopy-sha256")
        if args.objcopy:
            objcopy = verified_objcopy(args.objcopy, args.objcopy_sha256)
    zig = None
    toolchain_lock_path = None
    toolchain_lock = None
    archive = None
    if not args.native_tests:
        if not args.zig:
            parser.error("--zig is required for the cross build")
        zig = args.zig.resolve(strict=True)
        if args.zig_sha256 and digest(zig) != args.zig_sha256:
            raise ValueError("Zig executable pin mismatch")
        toolchain_lock_path = (args.toolchain_lock or default_toolchain_lock(host)).resolve(strict=True)
        toolchain_lock = json.loads(toolchain_lock_path.read_text())
        if toolchain_lock.get("host") != host or toolchain_lock.get("version") != "0.16.0":
            raise ValueError("locked compiler host/version differs from build host/policy")
        archive = (args.zig_archive or zig.parent.parent /
                   ("zig-" + host + "-0.16.0.tar.xz")).resolve(strict=True)
    parent = args.output_parent.resolve(strict=True) if args.output_parent else Path(tempfile.gettempdir()).resolve()
    if not parent.is_dir() or any(char.isspace() for char in str(parent)):
        raise ValueError("Autotools staging parent must be an existing space-free directory")
    stage = Path(tempfile.mkdtemp(prefix="hashstat-core-", dir=parent))
    receipt = {"schema": 1, "upstreamCommit": PIN, "source": str(source),
               "stage": str(stage), "sourceFiles": before,
               "zigExecutableSHA256": digest(zig) if zig else None,
               "host": host, "sourceLockSHA256": digest(args.source_lock) if args.source_lock else None,
               "officialArchiveSignatureVerifiedHere": False,
               "commands": [], "success": False, "hardwareLifecycleImplemented": False,
               "buildToolSHA256": digest(Path(__file__).resolve()),
               "mode": "native-core-tests" if args.native_tests else "cross-arm-core",
               "minerExecuted": False, "installed": False, "published": False,
               "miningStartupExecuted": False, "safeMainEntryPointsExecuted": [],
               "newDriver": "HashStat AML88 work/UART/core bridge; hardware startup owner still unavailable",
               "controllerOnlyStandbyImplemented": True,
               "fullApiCompatibility": False,
               "portableVerifierSHA256": digest(portable_verifier),
               "compilerPolicy": "Linux ARM hard-float static; -fcommon for legacy tentative driver globals"}
    started = time.monotonic()
    env = os.environ.copy()

    for name in ("CC", "CXX", "CPP", "AR", "RANLIB", "LD", "STRIP", "CFLAGS", "CXXFLAGS", "CPPFLAGS",
                 "LDFLAGS", "LIBS", "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "LIBRARY_PATH",
                 "PKG_CONFIG_PATH", "PKG_CONFIG_LIBDIR", "PKG_CONFIG_SYSROOT_DIR", "SDKROOT", "CONFIG_SITE",
                 "ASAN_OPTIONS", "UBSAN_OPTIONS", "LSAN_OPTIONS"):
        env.pop(name, None)
    env["PATH"] = ("/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin" if platform.system() == "Darwin"
                   else "/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin")
    env["LC_ALL"] = "C"
    env["CONFIG_SITE"] = "/dev/null"
    env["SOURCE_DATE_EPOCH"] = source_epoch
    env["TZ"] = "UTC"

    def run(name: str, command: list[str], cwd: Path, timeout: int = 240) -> None:
        record = {"stage": name, "argv": command, "cwd": str(cwd),
                  "log": str(stage / (name + ".log")), "returncode": None}
        receipt["commands"].append(record)
        print("BUILD_STAGE", name, "LOG", record["log"], flush=True)
        with open(record["log"], "xb") as log:
            try:
                done = subprocess.run(command, cwd=cwd, env=env, stdout=log,
                                      stderr=subprocess.STDOUT, timeout=timeout)
                record["returncode"] = done.returncode
            except subprocess.TimeoutExpired:
                record["timedOut"] = True
                raise
        if done.returncode != 0:
            raise RuntimeError(f"{name} failed with {done.returncode}; see {record['log']}")

    try:
        if not args.native_tests:
            verifier = Path(__file__).resolve().parent / "verify_zig_toolchain.py"
            tree_receipt = stage / "toolchain-tree-verification.json"
            run("verify-toolchain", [sys.executable, str(verifier), "--archive", str(archive),
                                    "--directory", str(zig.parent), "--output", str(tree_receipt),
                                    "--lock", str(toolchain_lock_path)], stage)
            verified = json.loads(tree_receipt.read_text())
            if (verified.get("status") != "PASSED" or
                    (host == "aarch64-macos" and verified.get("treeManifestSHA256") != ZIG_TREE_PIN)):
                raise ValueError("whole Zig tree pin mismatch")
            receipt["toolchainTree"] = {key: verified[key] for key in
                                        ("status", "lockSHA256", "archiveSHA256", "treeManifestSHA256", "fileCount", "fileBytes")}
            receipt["toolchainTree"]["receiptSHA256"] = digest(tree_receipt)
            receipt["toolchainVerifierSHA256"] = digest(verifier)
        snapshot = stage / "source"
        snapshot.mkdir()
        for item in before:
            dest = snapshot / item["path"]
            dest.parent.mkdir(parents=True, exist_ok=True)
            with (source / item["path"]).open("rb") as incoming, dest.open("xb") as outgoing:
                shutil.copyfileobj(incoming, outgoing)

            dest.chmod(0o755 if item["executable"] else 0o644)
        if metadata(names, snapshot) != before:
            raise ValueError("source changed during snapshot")
        wrappers = stage / "wrappers"
        wrappers.mkdir()
        pkgempty = stage / "target-pkgconfig-empty"
        pkgempty.mkdir()
        pkgconfig = shutil.which("pkg-config", path=env["PATH"])
        if not pkgconfig:
            raise ValueError("host pkg-config tool unavailable")
        python = sys.executable
        if " " in python or "\n" in python:
            raise ValueError("wrapper Python path must be space-free")
        targets = {}
        if not args.native_tests:
            targets = {
                "cc": [str(zig), "cc", "-target", "arm-linux-musleabihf", "-mcpu=cortex_a9",
                       "-mfpu=vfpv3-d16", "-mfloat-abi=hard", "-fcommon", "-static"],
                "ar": [str(zig), "ar"], "ranlib": [str(zig), "ranlib"],
            }


            maps = [(str(snapshot), "/usr/src/hashstat/cgminer"),
                    (str(stage), "/usr/src/hashstat/build"),
                    (str(zig.parent), "/opt/hashstat/zig0.16.0")]
            prefix_flags = [f"-f{kind}-prefix-map={old}={new}"
                            for old, new in maps for kind in ("file", "debug", "macro")]
            targets["cc"] += prefix_flags + ["-fdebug-compilation-dir=/usr/src/hashstat/cgminer"]
            receipt["prefixMaps"] = [{"from": old, "to": new} for old, new in maps]
        if args.native_tests:
            targets["cc"] = [native_compiler(args.native_cc, env["PATH"]), "-fcommon"]
            receipt["nativeCompiler"] = {"path": targets["cc"][0], "sha256": digest(Path(targets["cc"][0]))}
            for name, command in (("ar", "ar"), ("ranlib", "ranlib")):
                executable = shutil.which(command, path=env["PATH"])
                if not executable:
                    raise ValueError("native test prerequisite unavailable: " + command)
                targets[name] = [executable] + (["-fcommon"] if name == "cc" else [])
        for name, argv in targets.items():
            path = wrappers / name
            with path.open("x") as out:
                out.write(f"#!{python}\nimport os,sys\nargv={argv!r}+sys.argv[1:]\nos.execv(argv[0],argv)\n")
            path.chmod(0o755)
        pkgwrapper = wrappers / "pkg-config"
        with pkgwrapper.open("x") as out:
            out.write(f"#!{python}\nimport os,sys\nos.environ['PKG_CONFIG_PATH']=''\n"
                      f"os.environ['PKG_CONFIG_LIBDIR']={str(pkgempty)!r}\n"
                      "os.environ.pop('PKG_CONFIG_SYSROOT_DIR',None)\n"
                      f"os.execv({pkgconfig!r},[{pkgconfig!r}]+sys.argv[1:])\n")
        pkgwrapper.chmod(0o755)
        env.update(CC=str(wrappers / "cc"), AR=str(wrappers / "ar"),
                   RANLIB=str(wrappers / "ranlib"), PKG_CONFIG=str(pkgwrapper),
                   CFLAGS="-O2 -g -fno-strict-aliasing -fcommon", LDFLAGS="-static",
                   ZIG_GLOBAL_CACHE_DIR=str(stage / "zig-cache"))
        if args.native_tests:
            env.update(native_test_environment(platform.system()))
            receipt["compilerPolicy"] = "Native clang ASan/UBSan; test mains and explicit information-only early main"
            receipt["leakSanitizerEnabled"] = platform.system() == "Linux"
            receipt["sanitizerEnvironment"] = {name: env[name] for name in ("ASAN_OPTIONS", "UBSAN_OPTIONS")}
        receipt["hostTools"] = []
        for name in ("autoconf", "automake", "autoreconf", "make", "pkg-config", "perl", "m4"):
            executable = shutil.which(name, path=env["PATH"])
            if not executable:
                raise ValueError("host prerequisite unavailable: " + name)
            version = subprocess.run([executable, "--version"], env=env, text=True,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=10, check=True).stdout
            receipt["hostTools"].append({"name": name, "path": executable,
                                         "sha256": digest(Path(executable)), "version": version.splitlines()[:3]})
        compiler = targets["cc"][:2] if zig else targets["cc"][:1]
        run("compiler-version", compiler + ["--version"], stage, 30)
        receipt["buildEnvironment"] = {name: env[name] for name in
                                       ("CC", "AR", "RANLIB", "PKG_CONFIG", "CFLAGS", "LDFLAGS", "SOURCE_DATE_EPOCH")}
        run("autogen", ["/bin/sh", "./autogen.sh"], snapshot)
        run("build-triplet", ["/bin/sh", "./config.guess"], snapshot, 10)
        build_triplet = (stage / "build-triplet.log").read_text().strip()
        if not re.fullmatch(r"[a-zA-Z0-9_.+-]+", build_triplet) or "-" not in build_triplet:
            raise ValueError("invalid host config.guess result")
        if platform.system() == "Linux" and "linux" not in build_triplet:
            raise ValueError("config.guess did not identify the Linux build host")
        receipt["buildTriplet"] = build_triplet
        configure = ["/bin/sh", "./configure", "--build=" + build_triplet]
        if not args.native_tests:
            configure += ["--host=arm-linux-musleabihf"]
        configure += ["--enable-hashstat-aml88", "--disable-libcurl", "--without-curses", "--disable-libsystemd"]
        run("configure", configure, snapshot)
        run("make", ["make", "-j" + str(args.jobs)], snapshot, 900)
        if args.native_tests:
            test_names = ("hashstat-core-test", "hashstat-stratum-test", "hashstat-supervisor-test")
            run("make-tests", ["make", "-j" + str(args.jobs), *test_names], snapshot, 600)
            receipt["testArtifacts"] = []
            for name in test_names:
                binary = snapshot / name
                receipt["testArtifacts"].append({"path": str(binary), "sha256": digest(binary),
                                                 "size": binary.stat().st_size})
                run(name, [str(binary)], snapshot, 60)


            run("hashminer-build-info", [str(snapshot / "cgminer"), "--hashstat-build-info"], snapshot, 5)
            info = json.loads((stage / "hashminer-build-info.log").read_text())
            version = expected_version(snapshot)
            if (info.get("component") != "HashMiner" or info.get("version") != version
                    or info.get("hardwareStartupImplemented") is not False
                    or info.get("miningReady") is not False
                    or info.get("upstream", {}).get("commit") != PIN):
                raise ValueError("linked HashMiner provenance mismatch")
            run("hashminer-version", [str(snapshot / "cgminer"), "--version"], snapshot, 5)
            if not (stage / "hashminer-version.log").read_text().startswith("HashMiner " + version + " ("):
                raise ValueError("linked HashMiner version mismatch")
            receipt["minerExecuted"] = True
            receipt["safeMainEntryPointsExecuted"] = ["--hashstat-build-info", "--version"]
            receipt["componentInfo"] = info
            receipt["success"] = True
        else:
            diagnostic = snapshot / "cgminer"
            original = diagnostic.read_bytes()
            diagnostic_sha = digest(diagnostic)
            arm_elf_metadata(original)
            diagnostic_info = arm_elf_metadata(original)
            receipt["diagnosticArtifact"] = {"path": str(diagnostic), "size": len(original),
                                             "sha256": diagnostic_sha, **diagnostic_info,
                                             "reproducibleBytesClaimed": False}
            if objcopy:
                artifact = snapshot / "cgminer.release"
                if artifact.exists() or artifact.is_symlink():
                    raise ValueError("refuse existing release output")


                verified_objcopy(objcopy, args.objcopy_sha256)
                run("objcopy-version", [str(objcopy), "--version"], snapshot, 10)
                run("strip-release-debug", [str(objcopy), "--strip-debug", str(diagnostic), str(artifact)], snapshot, 30)
                if digest(diagnostic) != diagnostic_sha:
                    raise ValueError("diagnostic ELF changed during release transform")
                verified_objcopy(objcopy, args.objcopy_sha256)
                data = artifact.read_bytes()
                _, release_info = validate_release_pair(original, data)
                receipt["artifact"] = {"path": str(artifact), "size": len(data), "sha256": digest(artifact),
                                       **release_info, "role": "release-core-elf"}
                receipt["releaseTransform"] = {"allocatedSectionsUnchanged": True, "programHeadersUnchanged": True,
                                               "debugSectionsAbsent": True, "diagnosticPreserved": True,
                                               "method": "objcopy --strip-debug", "objcopy": str(objcopy),
                                               "objcopySHA256": args.objcopy_sha256, "objcopyUnchanged": True,
                                               "trustBasis": "explicit provisioned executable SHA256; system package closure is external"}
            else:


                receipt["artifact"] = {"path": str(diagnostic), "size": len(original), "sha256": diagnostic_sha,
                                       **diagnostic_info, "role": "legacy-diagnostic-core-elf"}
            receipt["success"] = True
        if zig:
            final_tree_receipt = stage / "toolchain-tree-after.json"
            run("verify-toolchain-after", [sys.executable, str(verifier), "--archive", str(archive),
                                          "--directory", str(zig.parent), "--output", str(final_tree_receipt),
                                          "--lock", str(toolchain_lock_path)], stage)
            final_tree = json.loads(final_tree_receipt.read_text())
            receipt["toolchainUnchanged"] = all(final_tree.get(key) == verified.get(key) for key in
                                                ("lockSHA256", "archiveSHA256", "treeManifestSHA256"))
            if not receipt["toolchainUnchanged"]:
                raise ValueError("toolchain changed during build")
    except Exception as error:
        receipt["success"] = False
        receipt["failure"] = f"{type(error).__name__}: {error}"
    finally:
        try:
            receipt["sourceAfter"] = metadata(names if source_lock else source_names(source), source)
            receipt["sourceUnchanged"] = receipt["sourceAfter"] == before
            if not receipt["sourceUnchanged"]:
                receipt["success"] = False
                receipt["failure"] = "Original source changed during build; artifact is not accepted"
        except Exception as error:
            receipt["success"] = False
            receipt["sourceReadbackFailure"] = str(error)
        receipt["elapsedSeconds"] = time.monotonic() - started
        with (stage / "receipt.json").open("x") as out:
            json.dump(receipt, out, indent=2, sort_keys=True)
            out.write("\n")
        print("RECEIPT", stage / "receipt.json", "SUCCESS", receipt["success"], flush=True)
    return 0 if receipt["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
