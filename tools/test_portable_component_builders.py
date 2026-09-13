import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parent.parent
NAMES = (
    "tools/build_aml_chain.py", "tools/build_aml_readonly_diagnostic.py",
    "tools/build_test_supervisor.py", "transport/build_linux_adapter.py",
)


class PortableComponentBuilders(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name).resolve()
        self.zig = self.folder / "zig"
        self.archive = self.folder / "archive.tar.xz"
        self.lock = self.folder / "lock.json"
        self.zig.write_bytes(b"fixture compiler; never executed")
        self.archive.write_bytes(b"fixture archive; never executed")
        self.modules = []
        for index, name in enumerate(NAMES):
            spec = importlib.util.spec_from_file_location("component_builder_" + str(index), ROOT / name)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            self.modules.append(module)

    def arguments(self):
        return ["--zig", str(self.zig), "--zig-archive", str(self.archive),
                "--toolchain-lock", str(self.lock), "--output-parent", str(self.folder),
                "--cc", sys.executable, "--readelf", sys.executable]

    def test_explicit_host_compilers_and_output(self):
        for module in self.modules:
            for host in ("x86_64-linux", "aarch64-linux", "aarch64-macos"):
                with self.subTest(module=module.__file__, host=host):
                    self.lock.write_text(json.dumps({"host": host, "version": "0.16.0"}))
                    with mock.patch.object(module, "host_identity", return_value=host), \
                            mock.patch.object(module.subprocess, "run", side_effect=AssertionError("no build")):
                        args = module.parse_options(self.arguments())
                    self.assertEqual(args.host, host)
                    self.assertEqual(args.zig, self.zig)
                    self.assertEqual(args.zig_archive, self.archive)
                    self.assertEqual(args.output_parent, self.folder)
                    self.assertEqual(args.cc, str(Path(sys.executable).resolve()))
                    self.assertEqual(args.readelf, str(Path(sys.executable).resolve()))

    def test_wrong_host_or_version_fails_before_build(self):
        for module in self.modules:
            for lock in ({"host": "aarch64-macos", "version": "0.16.0"},
                         {"host": "x86_64-linux", "version": "master"}):
                with self.subTest(module=module.__file__, lock=lock):
                    self.lock.write_text(json.dumps(lock))
                    with mock.patch.object(module, "host_identity", return_value="x86_64-linux"), \
                            self.assertRaisesRegex(ValueError, "host/version"):
                        module.parse_options(self.arguments())

    def test_default_linux_lock(self):
        for module in self.modules:
            with self.subTest(module=module.__file__):
                argv = self.arguments()
                index = argv.index("--toolchain-lock")
                del argv[index:index+2]
                with mock.patch.object(module, "host_identity", return_value="x86_64-linux"):
                    args = module.parse_options(argv)
                self.assertEqual(args.toolchain_lock,
                                 ROOT / "toolchains/zig-0.16.0-x86_64-linux.lock.json")

    def test_no_implicit_zig_compiler(self):
        for module in self.modules:
            with self.subTest(module=module.__file__), contextlib.redirect_stderr(io.StringIO()), \
                    self.assertRaises(SystemExit) as raised:
                module.parse_options([])
            self.assertEqual(raised.exception.code, 2)

    def test_help_does_not_read_inputs(self):
        for module in self.modules:
            with self.subTest(module=module.__file__), contextlib.redirect_stdout(io.StringIO()), \
                    mock.patch.object(module, "host_identity", side_effect=AssertionError("no host lookup")), \
                    self.assertRaises(SystemExit) as raised:
                module.parse_options(["--help"])
            self.assertEqual(raised.exception.code, 0)

    def test_missing_tools_and_bad_output_fail(self):
        self.lock.write_text(json.dumps({"host": "x86_64-linux", "version": "0.16.0"}))
        for module in self.modules:
            with self.subTest(module=module.__file__), \
                    mock.patch.object(module, "host_identity", return_value="x86_64-linux"):
                argv = self.arguments()
                argv[argv.index("--cc")+1] = "hashstat-no-such-compiler"
                with self.assertRaisesRegex(ValueError, "compiler.*readelf"):
                    module.parse_options(argv)
                argv = self.arguments()
                argv[argv.index("--output-parent")+1] = str(self.zig)
                with self.assertRaisesRegex(ValueError, "output parent"):
                    module.parse_options(argv)

    def test_source_closure_and_portable_commands(self):
        for module in self.modules:
            with self.subTest(module=module.__file__):
                for name in module.NAMES:
                    self.assertTrue((ROOT / name).is_file(), name)
                source = Path(module.__file__).read_text()
                self.assertNotIn('/private/tmp', source)
                self.assertNotIn('/opt/homebrew', source)
                self.assertNotIn('/usr/bin/clang', source)
                self.assertIn('"--lock", str(args.toolchain_lock)', source)
                self.assertIn('"-fno-sanitize-recover=all"', source)
                self.assertIn('args.host.endswith("-macos") else "1"', source)


if __name__ == "__main__":
    unittest.main()
