#!/usr/bin/env python3

import hashlib
import io
import json
from pathlib import Path
import subprocess
import struct
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

import build_cgminer_core as build


def elf_fixture(debug=True, text=b"\x00\x01\x02\x03"):
    names = b"\0.text\0.bss\0.debug_info\0.shstrtab\0"
    data = bytearray(256) + text
    sections = [(0,) * 10, (1, 1, 6, 0x10100, 256, 4, 0, 0, 4, 0),
                (7, 8, 3, 0x10104, 260, 4, 0, 0, 4, 0)]
    if debug:
        sections.append((12, 1, 0, 0, len(data), 8, 0, 0, 1, 0))
        data += b"debug123"
    strings_offset = len(data)
    data += names
    sections.append((24, 3, 0, 0, strings_offset, len(names), 0, 0, 1, 0))
    while len(data) % 4:
        data.append(0)
    shoff = len(data)
    for row in sections:
        data += struct.pack("<10I", *row)
    data[:7] = b"\x7fELF\x01\x01\x01"
    struct.pack_into("<HHIIIIIHHHHHH", data, 16, 2, 40, 1, 0x10100, 52, shoff,
                     0x05000400, 52, 32, 1, 40, len(sections), len(sections) - 1)
    struct.pack_into("<8I", data, 52, 1, 0, 0x10000, 0x10000, 260, 264, 5, 4096)
    return bytes(data)


class CoreBuildTests(unittest.TestCase):
    def test_debug_free_release_preserves_allocated_sections_and_mapping(self):
        before, after = build.validate_release_pair(elf_fixture(), elf_fixture(debug=False))
        self.assertEqual(before["debugSections"], [".debug_info"])
        self.assertEqual(after["debugSections"], [])
        self.assertEqual(before["allocatedSections"], after["allocatedSections"])
        self.assertEqual(before["programHeaders"], after["programHeaders"])

    def test_release_rejects_code_mapping_and_debug_drift(self):
        with self.assertRaisesRegex(ValueError, "allocated"):
            build.validate_release_pair(elf_fixture(), elf_fixture(False, b"xxxx"))
        with self.assertRaisesRegex(ValueError, "debug sections"):
            build.validate_release_pair(elf_fixture(), elf_fixture())
        changed = bytearray(elf_fixture(False))
        struct.pack_into("<I", changed, 24, 0x10104)
        with self.assertRaisesRegex(ValueError, "entry"):
            build.validate_release_pair(elf_fixture(), bytes(changed))

    def test_elf_section_bounds_rejected(self):
        broken = bytearray(elf_fixture(False))
        struct.pack_into("<I", broken, 32, len(broken) + 100)
        with self.assertRaisesRegex(ValueError, "section table"):
            build.arm_elf_metadata(bytes(broken))

    def test_objcopy_requires_exact_executable_pin(self):
        with tempfile.TemporaryDirectory(prefix="core-objcopy-test-") as name:
            tool = Path(name).resolve() / "objcopy"
            tool.write_bytes(b"synthetic fixture, never execute")
            tool.chmod(0o755)
            pin = build.digest(tool)
            self.assertEqual(build.verified_objcopy(tool, pin), tool)
            with self.assertRaises(ValueError):
                build.verified_objcopy(Path("relative-objcopy"), pin)
            tool.write_text("drift")
            with self.assertRaisesRegex(ValueError, "pin/type"):
                build.verified_objcopy(tool, pin)

    def test_explicit_native_compiler_bypasses_path_default(self):
        with tempfile.TemporaryDirectory(prefix="core-native-cc-test-") as name:
            compiler = Path(name).resolve() / "clang-21"
            compiler.write_text("synthetic fixture; never execute\n")
            compiler.chmod(0o755)
            self.assertEqual(build.native_compiler(compiler, "/no-default-compiler"), str(compiler))
            with self.assertRaisesRegex(ValueError, "absolute"):
                build.native_compiler(Path("clang-21"), "/usr/bin")
            compiler.chmod(0o644)
            with self.assertRaisesRegex(ValueError, "executable"):
                build.native_compiler(compiler, "/usr/bin")

    def test_native_sanitizers_linux_leaks_and_macos_compatibility(self):
        linux = build.native_test_environment("Linux")
        macos = build.native_test_environment("Darwin")
        self.assertIn("detect_leaks=1", linux["ASAN_OPTIONS"])
        self.assertIn("detect_leaks=0", macos["ASAN_OPTIONS"])
        for settings in (linux, macos):
            self.assertIn("-fno-sanitize-recover=all", settings["CFLAGS"])
            self.assertIn("halt_on_error=1", settings["ASAN_OPTIONS"])
            self.assertIn("halt_on_error=1", settings["UBSAN_OPTIONS"])

    def test_host_lock_mapping(self):
        for system, arch, wanted in (("Linux", "x86_64", "x86_64-linux"),
                                     ("Linux", "aarch64", "aarch64-linux"),
                                     ("Darwin", "arm64", "aarch64-macos")):
            host = build.host_identity(system, arch)
            self.assertEqual(host, wanted)
            lock = json.loads(build.default_toolchain_lock(host).read_text())
            self.assertEqual(lock["host"], wanted)
            self.assertEqual(lock["version"], "0.16.0")
            self.assertTrue(lock["url"].startswith("https://ziglang.org/download/0.16.0/zig-" + wanted))
        with self.assertRaises(ValueError):
            build.host_identity("Windows", "AMD64")

    def test_source_lock_git_free_and_drift(self):
        with tempfile.TemporaryDirectory(prefix="core-lock-test-") as name:
            root = Path(name)
            source = root / "source"
            source.mkdir()
            file = source / "autogen.sh"
            file.write_text("#!/bin/sh\nexit 0\n")
            file.chmod(0o755)
            lock = {"schema": "hashstat.cgminer-core-source.v1", "upstreamCommit": build.PIN,
                    "sourceDateEpoch": "1544130810", "sourceFiles": build.metadata(["autogen.sh"], source)}
            lock_path = root / "lock.json"
            lock_path.write_text(json.dumps(lock))
            with patch.object(build.subprocess, "check_output", side_effect=AssertionError("Git must not run")):
                rows, epoch, _ = build.source_inputs(source, lock_path)
                self.assertEqual(rows, lock["sourceFiles"])
                self.assertEqual(epoch, lock["sourceDateEpoch"])
                file.chmod(0o644)
                with self.assertRaisesRegex(ValueError, "differs"):
                    build.source_inputs(source, lock_path)
                file.chmod(0o755)
                file.write_text("modified\n")
                with self.assertRaisesRegex(ValueError, "differs"):
                    build.source_inputs(source, lock_path)

    def test_source_path_validation(self):
        with tempfile.TemporaryDirectory(prefix="core-path-test-") as name:
            root = Path(name)
            file = root / "real.c"
            file.write_text("test")
            (root / "link.c").symlink_to(file)
            for path in ("../real.c", "/real.c", "link.c", "./real.c", "a/../real.c", "a\\b", ".git/config"):
                with self.assertRaises(ValueError, msg=path):
                    build.metadata([path], root)

    def test_portable_verifies_requested_source(self):
        with tempfile.TemporaryDirectory(prefix="core-portable-test-") as name:
            source = Path(name)
            (source / "hashstat-pure").mkdir()
            rows = []
            for rel in build.PORTABLE:
                path = source / "hashstat-pure" / Path(rel).name
                path.write_text(rel)
                rows.append({"path": "hashstat-pure/" + path.name, "sha256": build.digest(path)})
            with patch.object(build, "ROOT", source / "unrelated-source-root"):
                self.assertEqual(build.verify_portable(source, {"portableFiles": rows}), rows)
                path.write_text("drift")
                with self.assertRaisesRegex(ValueError, "portable mirror"):
                    build.verify_portable(source, {"portableFiles": rows})

    def test_source_version_is_not_relabelled(self):
        with tempfile.TemporaryDirectory(prefix="core-version-test-") as name:
            root = Path(name)
            path = root / "hashstat-version.h"
            path.write_text('#define HASHSTAT_VERSION "0.0.1"\n')
            self.assertEqual(build.expected_version(root), "0.0.1")
            path.write_text('#define HASHSTAT_VERSION "0.0.1"\n#define HASHSTAT_VERSION "other"\n')
            with self.assertRaises(ValueError):
                build.expected_version(root)


class ToolchainTests(unittest.TestCase):
    def test_linux_archive_unpack_tree_and_drift(self):
        with tempfile.TemporaryDirectory(prefix="core-toolchain-test-") as name:
            root = Path(name)
            prefix = "zig-x86_64-linux-0.16.0"
            archive = root / (prefix + ".tar.xz")
            content = b"synthetic fixture, NEVER execute\n"
            with tarfile.open(archive, "w:xz") as stream:
                for rel, mode in (("zig", 0o755), ("lib/header.h", 0o644)):
                    info = tarfile.TarInfo(prefix + "/" + rel)
                    info.size, info.mode = len(content), mode
                    stream.addfile(info, io.BytesIO(content))
            lock = root / "fixture.lock.json"
            lock.write_text(json.dumps({"host": "x86_64-linux", "version": "0.16.0",
                                        "size": archive.stat().st_size, "sha256": build.digest(archive)}))
            command = [sys.executable, str(build.ROOT / "tools/unpack_zig_toolchain.py"),
                       "--archive", str(archive), "--parent", str(root), "--lock", str(lock)]
            subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
            verify = [sys.executable, str(build.ROOT / "tools/verify_zig_toolchain.py"),
                      "--archive", str(archive), "--directory", str(root / prefix), "--lock", str(lock)]
            subprocess.run(verify + ["--output", str(root / "verified.json")],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
            self.assertEqual(json.loads((root / "verified.json").read_text())["status"], "PASSED")
            (root / prefix / "lib/header.h").write_text("modified")
            result = subprocess.run(verify + ["--output", str(root / "drift.json")],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((root / "drift.json").exists())

            self.assertNotEqual(subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE).returncode, 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
