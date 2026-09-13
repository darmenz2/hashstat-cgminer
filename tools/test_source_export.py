import json
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch

import build_cgminer_core as build
import export_cgminer_core_source as export


class SourceExportTests(unittest.TestCase):
    def test_transient_payload_drift_cannot_produce_receipt(self):
        root = Path(__file__).resolve().parents[1]
        original = Path.read_bytes
        def changed(path):
            return b"transient changed bytes" if path == root / "Makefile" else original(path)
        with tempfile.TemporaryDirectory(prefix="hashstat-export-test-") as name:
            output = Path(name) / "source.tar.gz"
            with patch.object(Path, "read_bytes", changed), self.assertRaisesRegex(ValueError, "archive payload"):
                export.export(root / "core", root / "source-lock.json", output)
            self.assertFalse(Path(str(output) + ".receipt.json").exists())

    def test_output_cannot_mutate_checkout(self):
        root = Path(__file__).resolve().parents[1]
        for output in (root / "source-test.tar.gz", root / "core/source-test.tar.gz"):
            with self.assertRaisesRegex(ValueError, "outside source"):
                export.export(root / "core", root / "source-lock.json", output)
            self.assertFalse(output.exists())

    def test_source_only_export_uses_checkout_inputs(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(prefix="hashstat-export-test-") as name:
            output = Path(name) / "source.tar.gz"
            result = export.export(root / "core", root / "source-lock.json", output)
            self.assertTrue(result["sourceUnchanged"])
            with tarfile.open(output) as archive:
                names = {item.name for item in archive.getmembers()}
                self.assertIn("hashstat-core-source/core/hashstat-version.h", names)
                self.assertIn("hashstat-core-source/Makefile", names)
                self.assertIn("hashstat-core-source/runtime/hashminer_runtime.c", names)
                self.assertFalse(any(name.endswith((".md", ".class", ".ncd")) for name in names))
                self.assertFalse(any("peripheral/" in name for name in names))

    def test_support_lock_drift_rejected(self):
        root = Path(__file__).resolve().parents[1]
        lock = json.loads((root / "source-lock.json").read_text())
        lock["supportFiles"][0]["sha256"] = "0" * 64
        with tempfile.TemporaryDirectory(prefix="hashstat-export-test-") as name:
            temporary = Path(name)
            path = temporary / "source-lock.json"
            path.write_text(json.dumps(lock))
            with self.assertRaisesRegex(ValueError, "support source"):
                export.export(root / "core", path, temporary / "source.tar.gz")
            self.assertFalse((temporary / "source.tar.gz").exists())


if __name__ == "__main__":
    unittest.main()
