#!/usr/bin/env python3

import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("component_build", Path(__file__).with_name("build.py"))
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


class SanitizerGateTests(unittest.TestCase):
    def test_linux_enforces_leaks_and_halt_despite_inherited_options(self):
        inherited = {"ASAN_OPTIONS": "detect_leaks=0:halt_on_error=0",
                     "UBSAN_OPTIONS": "halt_on_error=0", "KEEP_ME": "unchanged"}
        with patch.dict(os.environ, inherited, clear=True), patch.object(sys, "platform", "linux"):
            env = build.native_sanitizer_environment()
            self.assertEqual(dict(os.environ), inherited)
        self.assertEqual(env["ASAN_OPTIONS"], "detect_leaks=1:halt_on_error=1")
        self.assertEqual(env["UBSAN_OPTIONS"], "halt_on_error=1:print_stacktrace=1")
        self.assertEqual(env["KEEP_ME"], "unchanged")

    def test_macos_disables_only_unsupported_leak_check(self):
        with patch.object(sys, "platform", "darwin"):
            env = build.native_sanitizer_environment()
        self.assertEqual(env["ASAN_OPTIONS"], "detect_leaks=0:halt_on_error=1")
        self.assertEqual(env["UBSAN_OPTIONS"], "halt_on_error=1:print_stacktrace=1")

    def test_real_sanitizer_diagnostics_are_nonzero(self):
        cc = shutil.which("clang")
        self.assertIsNotNone(cc, "clang required for the local sanitizer gate check")
        cases = {
            "ubsan": ("#include <limits.h>\nint main(void) { volatile int n = INT_MAX; "
                      "volatile int result = n + 1; (void)result; return 0; }\n", "runtime error:"),
            "asan": ("#include <stdlib.h>\nint main(void) { char *p = malloc(1); "
                     "if (!p) return 2; free(p); *(volatile char *)p = 1; return 0; }\n",
                     "ERROR: AddressSanitizer:"),
        }
        if sys.platform.startswith("linux"):
            cases["leak"] = ("#include <stdlib.h>\n__attribute__((noinline)) "
                             "static void leak(void) { volatile char *p = malloc(32); "
                             "if (p) *p = 1; }\nint main(void) { leak(); return 0; }\n",
                             "ERROR: LeakSanitizer:")

        stage = Path(tempfile.mkdtemp(prefix="hashstat-build-sanitizer-gate-"))
        print("sanitizer gate artifacts:", stage)
        for name, (source, marker) in cases.items():
            with self.subTest(name=name):
                binary = stage / name
                compiled = subprocess.run([cc, "-std=c11", "-O0", "-g"] + build.NATIVE_SANITIZER_FLAGS +
                                          ["-x", "c", "-", "-o", str(binary)], input=source,
                                          text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
                (stage / (name + "-compile.log")).write_text(compiled.stdout)
                self.assertEqual(compiled.returncode, 0, compiled.stdout)
                result = subprocess.run([str(binary)], env=build.native_sanitizer_environment(),
                                        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
                (stage / (name + "-run.log")).write_text(result.stdout)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn(marker, result.stdout)


if __name__ == "__main__":
    unittest.main()
