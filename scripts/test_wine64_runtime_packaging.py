#!/usr/bin/env python3
"""Packaging contract for the guest's wineserver executables.

Wine64 execs `/usr/lib/wine/wineserver` directly. The distro ships a 382-byte
/bin/sh wrapper under that name which picks between wineserver64 and
wineserver32. Packaged as the guest's wineserver, a 64-bit process exec'd a
shell script: BoxedWine resolved it through /bin/sh, took the ELF32 loader
path while the process was still marked 64-bit, skipped both the 32-bit stack
transition and the ELF64 stack builder, and reported success without
installing any usable image. The child then produced nothing and its parent
waited on it forever.

Both guest paths must hold the same real x86-64 executable. These tests drive
the packaging validator against synthetic archives, including one built to the
exact broken shape, so the contract is enforced without a Wine install.
"""

from __future__ import annotations

import hashlib
import os
import shutil
import struct
import subprocess
import tempfile
import unittest
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BUILDER = REPO / "scripts" / "build-wine64-runtime-ci.sh"
VALIDATOR = REPO / "scripts" / "validate-wine64-runtime.sh"

# The wrapper the distro installs at /usr/lib/wine/wineserver.
SHELL_WRAPPER = b"""#!/bin/sh -e
if [ -x "$(dirname "$0")/wineserver64" ]; then
    exec "$(dirname "$0")/wineserver64" "$@"
fi
exec "$(dirname "$0")/wineserver32" "$@"
"""


def elf(machine: int = 62, elf_class: int = 2, body: bytes = b"") -> bytes:
    """A minimal ELF header, enough for a header-only class/machine check."""
    header = bytearray(64)
    header[0:4] = b"\x7fELF"
    header[4] = elf_class
    header[5] = 1  # little endian
    header[6] = 1  # EI_VERSION
    struct.pack_into("<H", header, 16, 2)  # e_type = ET_EXEC
    struct.pack_into("<H", header, 18, machine)
    return bytes(header) + body


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class WineserverPackagingContract(unittest.TestCase):
    """The builder must never package the shell wrapper as a guest binary."""

    def setUp(self) -> None:
        self.builder = BUILDER.read_text(encoding="utf-8")

    def test_builder_packages_the_real_executable_at_both_paths(self) -> None:
        self.assertIn('copy_as "${WINE_SERVER}" /usr/lib/wine/wineserver64',
                      self.builder)
        self.assertIn('copy_as "${WINE_SERVER}" /usr/lib/wine/wineserver',
                      self.builder)

    def test_builder_no_longer_packages_the_distro_wrapper(self) -> None:
        # The old shape copied ${WINE_ROOT}/wineserver -- the shell wrapper --
        # straight into the generic guest path.
        self.assertNotIn('copy_as "${WINE_ROOT}/wineserver"', self.builder)

    def test_builder_selects_wineserver_by_elf_header(self) -> None:
        # A generic candidate is acceptable only when its own header proves it
        # is x86-64, never because it happens to be an executable file.
        self.assertIn("is_elf64_x86_64()", self.builder)
        self.assertIn("find_first_elf64_x86_64", self.builder)
        selection = self.builder[self.builder.index("WINE_SERVER=\"$("):]
        self.assertTrue(
            selection.startswith('WINE_SERVER="$(find_first_elf64_x86_64'),
            "wineserver selection must go through the ELF-header check",
        )


@unittest.skipUnless(shutil.which("bash") and shutil.which("unzip")
                     and shutil.which("od"),
                     "the packaging validator needs bash, unzip and od")
class WineserverArchiveValidation(unittest.TestCase):
    """Drive the real validator against synthetic layered archives."""

    def build_archives(self, directory: Path, wineserver_generic: bytes):
        server = elf(body=b"wineserver64 payload")
        glibc = directory / "glibc-rootfs64.zip"
        wine = directory / "wine64.zip"
        with zipfile.ZipFile(glibc, "w") as archive:
            archive.writestr("lib64/ld-linux-x86-64.so.2", elf())
            archive.writestr("lib/x86_64-linux-gnu/libc.so.6", elf())
            # Wine's server needs the modelled user's XDG runtime directory.
            archive.writestr("run/user/1000/", b"")
        with zipfile.ZipFile(wine, "w") as archive:
            archive.writestr("usr/lib/wine/wine64", elf(body=b"wine64"))
            archive.writestr("usr/lib/wine/wineserver64", server)
            archive.writestr("usr/lib/wine/wineserver", wineserver_generic)
            archive.writestr(
                "usr/lib/x86_64-linux-gnu/wine/x86_64-unix/winemetal.so", elf())
            archive.writestr(
                "usr/lib/x86_64-linux-gnu/wine/x86_64-windows/ntdll.dll", b"MZ")
        return glibc, wine, server

    def run_validator(self, directory: Path, glibc: Path, wine: Path) -> tuple:
        manifest = directory / "manifest.txt"
        manifest.write_text(
            "source=third_party/boxedwine64-audit/tools/rootfs64/build-wine64-zip.sh\n"
            "source_image=boxedwine64/wine64-debian:bookworm\n"
            f"glibc_sha256={sha256(glibc.read_bytes())}\n"
            f"wine_sha256={sha256(wine.read_bytes())}\n",
            encoding="utf-8", newline="\n",
        )
        completed = subprocess.run(
            ["bash", str(VALIDATOR), "--input", str(directory),
             "--manifest", str(manifest)],
            capture_output=True, text=True, timeout=120,
        )
        return completed.returncode, completed.stdout + completed.stderr

    def test_matching_elf64_wineservers_are_accepted(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(directory, server)
            code, output = self.run_validator(directory, glibc, wine)
            self.assertEqual(code, 0, output)
            self.assertIn("wineserver matches wineserver64", output)

    def test_the_old_shell_wrapper_shape_is_rejected(self) -> None:
        # Exactly the artifact that shipped: a 382-byte /bin/sh wrapper at
        # usr/lib/wine/wineserver beside a valid ELF64 wineserver64.
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            glibc, wine, _ = self.build_archives(directory, SHELL_WRAPPER)
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0,
                                "a shell wrapper at the generic wineserver "
                                "path must fail validation")
            self.assertIn("is not an ELF file", output)

    def test_a_32_bit_wineserver_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            glibc, wine, _ = self.build_archives(
                directory, elf(machine=3, elf_class=1, body=b"i386"))
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0)
            self.assertTrue("ELFCLASS64" in output or "EM_X86_64" in output,
                            output)

    def test_a_divergent_wineserver_is_rejected(self) -> None:
        # Both are valid x86-64 ELF files, but they are not the same binary.
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            glibc, wine, _ = self.build_archives(
                directory, elf(body=b"a different wineserver"))
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0)
            self.assertIn("differ", output)

    def test_a_missing_generic_wineserver_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc = directory / "glibc-rootfs64.zip"
            wine = directory / "wine64.zip"
            with zipfile.ZipFile(glibc, "w") as archive:
                archive.writestr("lib64/ld-linux-x86-64.so.2", elf())
                archive.writestr("lib/x86_64-linux-gnu/libc.so.6", elf())
                archive.writestr("run/user/1000/", b"")
            with zipfile.ZipFile(wine, "w") as archive:
                archive.writestr("usr/lib/wine/wine64", elf())
                archive.writestr("usr/lib/wine/wineserver64", server)
                archive.writestr(
                    "usr/lib/x86_64-linux-gnu/wine/x86_64-unix/winemetal.so",
                    elf())
                archive.writestr(
                    "usr/lib/x86_64-linux-gnu/wine/x86_64-windows/ntdll.dll",
                    b"MZ")
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0)
            self.assertIn("usr/lib/wine/wineserver", output)


    def test_a_missing_guest_runtime_directory_is_rejected(self) -> None:
        # Wine chooses /run/user/1000 for its server socket directory. If the
        # rootfs does not ship it, wineserver cannot start.
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc = directory / "glibc-rootfs64.zip"
            wine = directory / "wine64.zip"
            with zipfile.ZipFile(glibc, "w") as archive:
                archive.writestr("lib64/ld-linux-x86-64.so.2", elf())
                archive.writestr("lib/x86_64-linux-gnu/libc.so.6", elf())
            with zipfile.ZipFile(wine, "w") as archive:
                archive.writestr("usr/lib/wine/wine64", elf())
                archive.writestr("usr/lib/wine/wineserver64", server)
                archive.writestr("usr/lib/wine/wineserver", server)
                archive.writestr(
                    "usr/lib/x86_64-linux-gnu/wine/x86_64-unix/winemetal.so",
                    elf())
                archive.writestr(
                    "usr/lib/x86_64-linux-gnu/wine/x86_64-windows/ntdll.dll",
                    b"MZ")
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0)
            self.assertIn("run/user/1000", output)


class GuestRuntimeDirectoryStaging(unittest.TestCase):
    """The builder has to stage the runtime directory into the rootfs layer."""

    def test_builder_stages_the_guest_runtime_directory(self) -> None:
        builder = BUILDER.read_text(encoding="utf-8")
        self.assertIn('"${STAGE}/run/user/1000"', builder)
        # It has to land in the rootfs archive, not the Wine archive.
        rootfs_line = [line for line in builder.splitlines()
                       if "glibc-rootfs64.zip" in line and "zip -qry9" in line]
        self.assertTrue(rootfs_line, "rootfs archive command not found")
        index = builder.index(rootfs_line[0])
        self.assertIn(" run ", builder[index:index + 200])


if __name__ == "__main__":
    unittest.main()
