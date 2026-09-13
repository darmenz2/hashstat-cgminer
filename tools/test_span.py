#!/usr/bin/env python3

import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

from build import NATIVE_SANITIZER_FLAGS, native_sanitizer_environment

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description="Build and run address-range tests.")
    parser.add_argument("--cc", default="clang")
    args = parser.parse_args()
    compiler = shutil.which(args.cc)
    if compiler is None:
        parser.error("compiler not found: " + args.cc)
    with tempfile.TemporaryDirectory(prefix="hashminer-span-") as directory:
        binary = Path(directory) / "test_span"
        command = [compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Wconversion",
                   "-Werror", "-pedantic", *NATIVE_SANITIZER_FLAGS,
                   "-I", str(ROOT / "include"), str(ROOT / "tests/test_span.c"),
                   "-o", str(binary)]
        subprocess.run(command, check=True, timeout=60)
        subprocess.run([str(binary)], check=True, timeout=30,
                       env=native_sanitizer_environment())


if __name__ == "__main__":
    main()
