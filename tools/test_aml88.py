#!/usr/bin/env python3
"""Build and run the AML88 protocol emulator; never opens hardware devices."""
import os
import pathlib
import shutil
import subprocess
import tempfile
ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCES = ["tests/test_aml88_start.c", "src/aml88_start.c", "src/bm1362_commands.c",
           "src/bm1362_setup.c", "src/bm1362_pll.c", "src/aml_frame.c",
           "transport/src/bm1362_integrity.c"]
def main():
    cc = shutil.which(os.environ.get("CC", "clang"))
    if not cc:
        raise SystemExit("A C11 compiler is required")
    with tempfile.TemporaryDirectory(prefix="hashstat-aml88-test-") as temp:
        for name, extra in (("native", []), ("sanitized", ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])):
            exe = pathlib.Path(temp) / name
            subprocess.run([cc, "-std=c11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
                            "-Iinclude", "-Itransport/include", *extra, *SOURCES, "-o", str(exe)], cwd=ROOT, check=True, timeout=120)
            subprocess.run([str(exe)], cwd=ROOT, check=True, timeout=60)
if __name__ == "__main__":
    main()
