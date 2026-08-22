#!/usr/bin/env python3
"""Host-independent tests for the DXMT graphics-manifest boundary."""

from __future__ import annotations

import hashlib
import importlib.util
import pathlib
import sys
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).with_name("validate-dxmt-guest-abi.py")
SPEC = importlib.util.spec_from_file_location("validate_dxmt_guest_abi", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class GraphicsManifestTests(unittest.TestCase):
    def write_manifest(self, root: pathlib.Path, *, duplicate: bool = False) -> pathlib.Path:
        lines = [
            "format=boxedvn-x64-graphics-v1",
            "probe=boxedvn-d3d11-cube-x64.exe",
            "probe_sha256=" + "0" * 64,
            "d3d11_sha256=" + "1" * 64,
            "dxgi_sha256=" + "2" * 64,
            "d3d10core_sha256=" + "3" * 64,
            "winemetal_sha256=" + "4" * 64,
            "native_archive=libdxmt_combined.a",
            "native_archive_sha256=" + "5" * 64,
        ]
        if duplicate:
            lines.append("probe_sha256=" + "6" * 64)
        path = root / "x64-graphics.manifest"
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return path

    def test_manifest_parser_accepts_complete_v1_surface(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = self.write_manifest(pathlib.Path(directory))
            values = MODULE.parse_key_value_manifest(path)
            self.assertEqual(values["format"], "boxedvn-x64-graphics-v1")
            self.assertEqual(set(values), MODULE.GRAPHICS_MANIFEST_KEYS)

    def test_manifest_parser_rejects_duplicate_keys(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = self.write_manifest(pathlib.Path(directory), duplicate=True)
            with self.assertRaisesRegex(MODULE.ValidationError, "duplicate"):
                MODULE.parse_key_value_manifest(path)

    def test_manifest_file_rejects_checksum_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            payload = root / "payload.bin"
            payload.write_bytes(b"verified payload")
            wrong = hashlib.sha256(b"different payload").hexdigest()
            with self.assertRaisesRegex(MODULE.ValidationError, "SHA-256 mismatch"):
                MODULE.validate_manifest_file(root, payload.name, wrong, "payload")


if __name__ == "__main__":
    unittest.main()
