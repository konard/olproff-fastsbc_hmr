#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later

from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path


def load_updater():
    repo_root = (
        Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
    )
    script = repo_root / "scripts" / "update_license_header.py"
    spec = importlib.util.spec_from_file_location("update_license_header", script)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


lic = load_updater()


class LicenseHeaderTest(unittest.TestCase):
    def assert_has_header(self, text: str) -> None:
        self.assertIn("Copyright (C) 2026  fastsbc_hmr contributors", text)
        self.assertIn("GNU General Public License", text)

    def test_adds_header_to_file_without_header(self) -> None:
        updated, status, reason = lic.update_text('#include <cstdio>\nint main() { return 0; }\n')

        self.assertEqual(status, "added")
        self.assertEqual(reason, "")
        self.assertTrue(updated.startswith("// fastsbc_hmr - "))
        self.assert_has_header(updated)
        self.assertIn("\n\n#include <cstdio>\n", updated)

    def test_updates_existing_project_header_idempotently(self) -> None:
        old = (
            "// fastsbc_hmr old text\n"
            "// Copyright (C) 2025  fastsbc_hmr contributors\n"
            "// old license body\n"
            "\n"
            "int value = 1;\n"
        )

        updated, status, _ = lic.update_text(old)
        updated_again, status_again, _ = lic.update_text(updated)

        self.assertEqual(status, "updated")
        self.assertEqual(status_again, "unchanged")
        self.assert_has_header(updated)
        self.assertNotIn("2025", updated)
        self.assertEqual(updated, updated_again)

    def test_inserts_after_pragma_once(self) -> None:
        updated, status, _ = lic.update_text("#pragma once\n\nint value();\n")

        self.assertEqual(status, "added")
        self.assertTrue(updated.startswith("#pragma once\n\n// fastsbc_hmr - "))
        self.assertIn("\n\nint value();\n", updated)

    def test_inserts_after_include_guard(self) -> None:
        source = "#ifndef SAMPLE_HPP\n#define SAMPLE_HPP\n\nint value();\n#endif\n"

        updated, status, _ = lic.update_text(source)

        self.assertEqual(status, "added")
        self.assertTrue(
            updated.startswith("#ifndef SAMPLE_HPP\n#define SAMPLE_HPP\n\n// fastsbc_hmr - ")
        )
        self.assertIn("\n\nint value();\n#endif\n", updated)

    def test_skips_third_party_copyright(self) -> None:
        source = "// Copyright (C) 2026  Other Project\nint value();\n"

        updated, status, reason = lic.update_text(source)

        self.assertEqual(status, "skipped")
        self.assertIn("third-party", reason)
        self.assertEqual(updated, source)

    def test_skips_marker(self) -> None:
        source = "// NO_AUTO_LICENSE_UPDATE\nint value();\n"

        updated, status, reason = lic.update_text(source)

        self.assertEqual(status, "skipped")
        self.assertEqual(reason, lic.SKIP_MARKER)
        self.assertEqual(updated, source)

    def test_removes_legacy_spdx_license_line(self) -> None:
        source = "// SPDX-License-Identifier: MIT\n//\n// file note\n\nint value();\n"

        updated, status, _ = lic.update_text(source)

        self.assertEqual(status, "added")
        self.assert_has_header(updated)
        self.assertNotIn("SPDX-License-Identifier: MIT", updated)
        self.assertIn("// file note\n", updated)

    def test_excludes_configured_directories(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            keep = root / "src"
            keep.mkdir()
            excluded = [
                root / ".antlr",
                root / "builddir",
                root / "subprojects",
                root / "third_party",
            ]
            for directory in excluded:
                directory.mkdir()
                (directory / "skip.cpp").write_text("int skip;\n", encoding="utf-8")
            (keep / "add.cpp").write_text("int add;\n", encoding="utf-8")

            summary, _ = lic.process_tree(root, dry_run=False)

            self.assertEqual(summary.processed, 1)
            self.assertEqual(summary.added, 1)
            self.assert_has_header((keep / "add.cpp").read_text(encoding="utf-8"))
            for directory in excluded:
                self.assertEqual(
                    (directory / "skip.cpp").read_text(encoding="utf-8"), "int skip;\n"
                )

    def test_preserves_crlf_line_endings(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "sample.cpp"
            path.write_bytes(b"#include <cstdio>\r\nint main() { return 0; }\r\n")

            result = lic.process_file(path, dry_run=False)
            data = path.read_bytes()

            self.assertEqual(result.status, "added")
            self.assertIn(b"\r\n// Copyright (C) 2026  fastsbc_hmr contributors\r\n", data)
            self.assertIn(b"\r\n#include <cstdio>\r\n", data)
            self.assertNotIn(b"\n", data.replace(b"\r\n", b""))


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
