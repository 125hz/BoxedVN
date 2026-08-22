#!/usr/bin/env python3
"""Host-independent tests for the DXMT graphics-manifest boundary."""

from __future__ import annotations

import hashlib
import importlib.util
import pathlib
import sys
import tempfile
import unittest
from unittest import mock


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


class PeSurfaceTests(unittest.TestCase):
    class FakeImage:
        def __init__(self, machine: int = 0x8664,
                     exports: dict[str, int] | None = None,
                     imports: dict[str, set[str | int]] | None = None):
            self.machine = machine
            self._exports = exports or {}
            self._imports = imports or {}

        def exports(self) -> dict[str, int]:
            return self._exports

        def import_symbols(self) -> dict[str, set[str | int]]:
            return self._imports

    def test_d3d11_surface_accepts_required_exports_and_imports(self) -> None:
        requirements = MODULE.PE_REQUIREMENTS["d3d11.dll"]
        image = self.FakeImage(
            exports=dict(requirements["exports"]),
            imports={
                "dxgi.dll": {"CreateDXGIFactory1"},
                "winemetal.dll": set(),
            },
        )
        with mock.patch.object(MODULE, "PEImage", return_value=image):
            MODULE.validate_pe(pathlib.Path("d3d11.dll"), requirements)

    def test_surface_rejects_non_amd64_machine(self) -> None:
        requirements = MODULE.PE_REQUIREMENTS["d3d11.dll"]
        image = self.FakeImage(
            machine=0xAA64,
            exports=dict(requirements["exports"]),
            imports={"dxgi.dll": {"CreateDXGIFactory1"},
                     "winemetal.dll": set()},
        )
        with mock.patch.object(MODULE, "PEImage", return_value=image):
            with self.assertRaisesRegex(MODULE.ValidationError, "AMD64"):
                MODULE.validate_pe(pathlib.Path("d3d11.dll"), requirements)

    def test_surface_rejects_wrong_export_ordinal(self) -> None:
        requirements = MODULE.PE_REQUIREMENTS["dxgi.dll"]
        exports = dict(requirements["exports"])
        exports["CreateDXGIFactory1"] = 99
        image = self.FakeImage(exports=exports,
                               imports={"winemetal.dll": set()})
        with mock.patch.object(MODULE, "PEImage", return_value=image):
            with self.assertRaisesRegex(MODULE.ValidationError,
                                         "expected 10"):
                MODULE.validate_pe(pathlib.Path("dxgi.dll"), requirements)

    def test_surface_rejects_missing_key_import_symbol(self) -> None:
        requirements = MODULE.PE_REQUIREMENTS["d3d11.dll"]
        image = self.FakeImage(
            exports=dict(requirements["exports"]),
            imports={"dxgi.dll": set(), "winemetal.dll": set()},
        )
        with mock.patch.object(MODULE, "PEImage", return_value=image):
            with self.assertRaisesRegex(MODULE.ValidationError,
                                         "CreateDXGIFactory1"):
                MODULE.validate_pe(pathlib.Path("d3d11.dll"), requirements)


if __name__ == "__main__":
    unittest.main()
