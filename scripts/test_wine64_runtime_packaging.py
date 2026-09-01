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
import posixpath
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


# Wine searches for its font backend at both multiarch paths in turn. On the
# runner /lib is a symlink to /usr/lib, but the guest archive has no symlinks,
# so a single staged copy satisfies only one of the two lookups.
#
# The two paths land in different archives: the builder partitions the stage by
# top-level directory, so lib/ goes to the glibc layer and usr/ to the Wine
# layer. Both are extracted into the same guest root, which is why one copy in
# each is the correct shape rather than a duplicate.
FREETYPE_GLIBC_PATH = "lib/x86_64-linux-gnu/libfreetype.so.6"
FREETYPE_WINE_PATH = "usr/lib/x86_64-linux-gnu/libfreetype.so.6"

# The X11 client libraries winex11.so links against directly. Without them the
# user driver cannot load and the process has no desktop window at all.
X11_CORE_LIBS = ("libX11.so.6", "libXext.so.6")
# Fontconfig loads without its configuration and then reports that it cannot
# load a default config file, which leaves Wine with no usable font backend.
FONTCONFIG_PATH = "etc/fonts/fonts.conf"
# Wine's own bitmap fonts, which the non-client area of a window is drawn with.
WINE_FONTS = ("vgasys.fon", "vgaoem.fon", "vgafix.fon")


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


ROOT = "usr/lib/x86_64-linux-gnu/wine"
PE_DIR = ROOT + "/x86_64-windows"
DATA_ROOT = "usr/share/wine"
DERIVED_DATA_ROOT = "usr/lib/share/wine"


def pe(machine: int = 0x8664, body: bytes = b"") -> bytes:
    """A minimal PE32+ AMD64 image: DOS header, e_lfanew, PE signature, COFF.

    The validator has to distinguish a real builtin from a truncated entry and
    from the text of a link target stored as a plain file, so a two-byte "MZ"
    is deliberately not enough to pass.
    """
    lfanew = 0x40
    dos = bytearray(b"MZ" + b"\0" * (lfanew - 2))
    dos[0x3c:0x40] = lfanew.to_bytes(4, "little")
    coff = b"PE\0\0" + machine.to_bytes(2, "little") + b"\0" * 18
    return bytes(dos) + coff + body


def guest_link(target: str) -> bytes:
    """BoxedWine reads a guest symlink from a `.link` entry's contents."""
    return target.encode("utf-8")


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class WineserverPackagingContract(unittest.TestCase):
    """The builder must never package the shell wrapper as a guest binary."""

    def setUp(self) -> None:
        self.builder = BUILDER.read_text(encoding="utf-8")

    def test_builder_packages_the_real_executable_at_both_paths(self) -> None:
        self.assertIn(
            'copy_as "${WINE_SERVER}" "${WINE_MODULE_ROOT}/wineserver64"',
            self.builder)
        self.assertIn(
            'copy_as "${WINE_SERVER}" "${WINE_MODULE_ROOT}/wineserver"',
            self.builder)

    def test_builder_puts_the_loader_in_the_module_tree(self) -> None:
        # Wine reads /proc/self/exe to decide where wineserver is, and takes
        # its module root from ntdll.so's parent. A loader packaged outside the
        # module tree makes those two derivations name different places, which
        # is how a run reached Windows code and then failed to load kernel32.
        self.assertIn('WINE_MODULE_ROOT="/usr/lib/x86_64-linux-gnu/wine"',
                      self.builder)
        self.assertIn('copy_as "${WINE64}" "${WINE_MODULE_ROOT}/wine64"',
                      self.builder)
        self.assertNotIn('copy_as "${WINE64}" /usr/lib/wine/wine64',
                         self.builder)

    def test_builder_writes_compatibility_paths_as_guest_links(self) -> None:
        # BoxedWine's ZIP filesystem does not read POSIX symlinks out of an
        # archive: it recognises a link by a `.link` suffix whose contents are
        # the target. `ln -s` here produced a small regular file holding the
        # target text, which is a silent failure.
        self.assertIn('> "${STAGE}${link}.link"', self.builder)
        self.assertNotIn("ln -s ", self.builder)
        for link in ("/usr/lib/wine/wine64", "/usr/bin/wine64",
                     "/usr/lib/wine/x86_64-windows"):
            self.assertIn("guest_link \"${WINE_MODULE_ROOT}/", self.builder)
            self.assertIn(link, self.builder)

    def test_builder_links_wines_derived_data_root_to_packaged_nls(self) -> None:
        self.assertIn("cp -aL /usr/share/wine", self.builder)
        self.assertIn(
            "guest_link /usr/share/wine /usr/lib/share/wine",
            self.builder,
        )

    def test_derived_data_root_matches_wines_real_request(self) -> None:
        requested = posixpath.normpath("/" + ROOT + "/../../share/wine")
        self.assertEqual(requested, "/" + DERIVED_DATA_ROOT)

    def test_builder_refuses_an_archive_without_the_builtins(self) -> None:
        # kernel32.dll present in the archive but unreachable is the failure
        # this layout exists to prevent; kernel32.dll absent would be worse.
        for builtin in ("ntdll.dll", "kernel32.dll", "kernelbase.dll"):
            self.assertIn(builtin, self.builder)
        self.assertIn("is not a PE image", self.builder)

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

    def build_archives(self, directory: Path, wineserver_generic: bytes,
                       freetype: bytes = None,
                       glibc_freetype: bool = True,
                       wine_freetype: bool = True,
                       x11_driver: bool = True):
        server = elf(body=b"wineserver64 payload")
        if freetype is None:
            freetype = elf(body=b"freetype payload")
        self._freetype = freetype
        self._wine_freetype = wine_freetype
        self._x11_driver = x11_driver
        glibc = directory / "glibc-rootfs64.zip"
        wine = directory / "wine64.zip"
        with zipfile.ZipFile(glibc, "w") as archive:
            archive.writestr("lib64/ld-linux-x86-64.so.2", elf())
            archive.writestr("lib/x86_64-linux-gnu/libc.so.6", elf())
            # Wine dlopens FreeType by soname and searches both multiarch
            # paths. A BoxedWine ZIP does not interpret POSIX symlinks, so
            # both have to be real files; this is the glibc layer's copy.
            if glibc_freetype:
                archive.writestr(FREETYPE_GLIBC_PATH, freetype)
            archive.writestr(FONTCONFIG_PATH, b"<fontconfig/>\n")
            for x11_lib in X11_CORE_LIBS:
                archive.writestr("lib/x86_64-linux-gnu/" + x11_lib, elf())
            # Wine's server needs the modelled user's XDG runtime directory.
            archive.writestr("run/user/1000/", b"")
        with zipfile.ZipFile(wine, "w") as archive:
            self.write_wine_layout(archive, server, wineserver_generic)
        return glibc, wine, server

    def write_wine_layout(self, archive, server: bytes,
                          wineserver_generic: bytes) -> None:
        """The canonical layout: loader and modules in one tree.

        Wine takes the directory it looks for wineserver in from
        /proc/self/exe and its module root from ntdll.so's parent, so the
        loader has to live in the same tree as the modules it loads.
        """
        # The Wine layer carries the /usr copy of the font backend, the X11
        # client libraries, the user driver and Wine's bitmap fonts.
        if getattr(self, "_wine_freetype", True):
            archive.writestr(FREETYPE_WINE_PATH,
                             getattr(self, "_freetype", elf(body=b"freetype")))
        for x11_lib in X11_CORE_LIBS:
            archive.writestr("usr/lib/x86_64-linux-gnu/" + x11_lib, elf())
        if getattr(self, "_x11_driver", True):
            archive.writestr(PE_DIR + "/winex11.drv", pe(body=b"winex11"))
            archive.writestr(ROOT + "/x86_64-unix/winex11.so", elf())
        for wine_font in WINE_FONTS:
            archive.writestr(DATA_ROOT + "/fonts/" + wine_font, b"fon:" + wine_font.encode())
        archive.writestr(ROOT + "/wine64", elf(body=b"wine64"))
        archive.writestr(ROOT + "/wineserver64", server)
        archive.writestr(ROOT + "/wineserver", wineserver_generic)
        archive.writestr(ROOT + "/x86_64-unix/winemetal.so", elf())
        for builtin in ("ntdll.dll", "kernel32.dll", "kernelbase.dll"):
            archive.writestr(PE_DIR + "/" + builtin, pe(body=builtin.encode()))
        for nls in ("c_20127.nls", "locale.nls", "l_intl.nls"):
            archive.writestr(DATA_ROOT + "/nls/" + nls, b"nls:" + nls.encode())
        for link, target in self.compatibility_links():
            archive.writestr(link + ".link", guest_link(target))

    @staticmethod
    def compatibility_links():
        return (
            ("usr/lib/wine/wine64", "/" + ROOT + "/wine64"),
            ("usr/lib/wine/wineserver", "/" + ROOT + "/wineserver"),
            ("usr/lib/wine/wineserver64", "/" + ROOT + "/wineserver64"),
            ("usr/lib/wine/x86_64-windows", "/" + PE_DIR),
            ("usr/lib/wine/x86_64-unix", "/" + ROOT + "/x86_64-unix"),
            ("usr/bin/wine64", "/" + ROOT + "/wine64"),
            ("usr/bin/wineserver", "/" + ROOT + "/wineserver"),
            (DERIVED_DATA_ROOT, "/" + DATA_ROOT),
        )

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
                # What the validator now requires in this layer.
                archive.writestr(FREETYPE_GLIBC_PATH, elf(body=b"freetype"))
                archive.writestr(FONTCONFIG_PATH, b"<fontconfig/>\n")
                for x11_lib in X11_CORE_LIBS:
                    archive.writestr("lib/x86_64-linux-gnu/" + x11_lib, elf())
                archive.writestr("run/user/1000/", b"")
            with zipfile.ZipFile(wine, "w") as archive:
                self.write_wine_layout(archive, server, server)
            self.rewrite_wine_archive(wine, drop=ROOT + "/wineserver")
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0)
            self.assertIn("wineserver", output)


    def test_a_missing_builtin_is_rejected(self) -> None:
        for missing in ("kernel32.dll", "kernelbase.dll", "ntdll.dll"):
            with self.subTest(missing=missing):
                with tempfile.TemporaryDirectory() as raw:
                    directory = Path(raw)
                    server = elf(body=b"wineserver64 payload")
                    glibc, wine, _ = self.build_archives(directory, server)
                    self.rewrite_wine_archive(wine, drop=PE_DIR + "/" + missing)
                    code, output = self.run_validator(directory, glibc, wine)
                    self.assertNotEqual(code, 0, output)
                    self.assertIn(missing, output)

    def test_missing_nls_data_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(directory, server)
            self.rewrite_wine_archive(
                wine, drop=DATA_ROOT + "/nls/locale.nls")
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0, output)
            self.assertIn("locale.nls", output)

    def test_missing_derived_data_root_link_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(directory, server)
            self.rewrite_wine_archive(wine, drop=DERIVED_DATA_ROOT + ".link")
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0, output)
            self.assertIn(DERIVED_DATA_ROOT, output)

    def test_a_builtin_that_is_not_a_pe_image_is_rejected(self) -> None:
        # Two bytes of "MZ" used to be enough. A truncated entry, or the text
        # of a link target stored as a plain file, must not pass for a module.
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(directory, server)
            self.rewrite_wine_archive(
                wine, replace={PE_DIR + "/kernel32.dll": b"MZ"})
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0, output)
            self.assertIn("kernel32.dll", output)

    def test_a_32_bit_builtin_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(directory, server)
            self.rewrite_wine_archive(
                wine, replace={PE_DIR + "/kernel32.dll": pe(machine=0x014c)})
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0, output)
            self.assertIn("AMD64", output)

    def test_a_posix_symlink_is_not_a_guest_link(self) -> None:
        # The shape that shipped: `ln -s` stored as a real symlink, which
        # BoxedWine reads as a regular file holding the target text.
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(directory, server)
            self.rewrite_wine_archive(
                wine, drop="usr/bin/wine64.link",
                add={"usr/bin/wine64": b"/" + ROOT.encode() + b"/wine64"})
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0, output)
            self.assertIn("usr/bin/wine64", output)

    def test_a_guest_link_to_nothing_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(directory, server)
            self.rewrite_wine_archive(
                wine,
                replace={"usr/bin/wine64.link": b"/usr/lib/wine/not-packaged"})
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0, output)
            self.assertIn("usr/bin/wine64", output)

    def rewrite_wine_archive(self, wine: Path, drop: str = "",
                             replace=None, add=None) -> None:
        """Rebuild the Wine archive with one entry dropped, changed or added."""
        replace = replace or {}
        add = add or {}
        with zipfile.ZipFile(wine) as archive:
            entries = [(item.filename, archive.read(item.filename))
                       for item in archive.infolist()]
        with zipfile.ZipFile(wine, "w") as archive:
            for name, data in entries:
                if name == drop:
                    continue
                archive.writestr(name, replace.get(name, data))
            for name, data in add.items():
                archive.writestr(name, data)

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
                # What the validator now requires in this layer.
                archive.writestr(FREETYPE_GLIBC_PATH, elf(body=b"freetype"))
                archive.writestr(FONTCONFIG_PATH, b"<fontconfig/>\n")
                for x11_lib in X11_CORE_LIBS:
                    archive.writestr("lib/x86_64-linux-gnu/" + x11_lib, elf())
            with zipfile.ZipFile(wine, "w") as archive:
                self.write_wine_layout(archive, server, server)
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


@unittest.skipUnless(shutil.which("bash") and shutil.which("unzip")
                     and shutil.which("od"),
                     "the packaging validator needs bash, unzip and od")
class FreeTypePackaging(unittest.TestCase):
    """Wine's font backend has to be in the archive, and has to be loadable.

    Two device runs reported "Wine cannot find the FreeType font library" after
    failing to open libfreetype.so.6 at both multiarch paths in turn. The
    archive simply did not contain it: nothing named it as a dynamically loaded
    dependency, so nothing missed it.
    """

    def test_builder_seeds_freetype_as_a_required_dynamic_library(self) -> None:
        builder = BUILDER.read_text(encoding="utf-8")
        # A reusable mechanism, not another bare copy: the seed feeds the ldd
        # closure so FreeType's own dependencies come from the runner.
        self.assertIn("seed_dynamic_library()", builder)
        self.assertIn("seed_dynamic_library libfreetype.so.6 required", builder)
        # Required means the build fails without it.
        self.assertIn("requirement}\" == \"required\"", builder)

    def test_builder_stages_dynamic_libraries_at_both_multiarch_paths(self) -> None:
        builder = BUILDER.read_text(encoding="utf-8")
        self.assertIn('copy_as "${found}" "/lib/x86_64-linux-gnu/${soname}"',
                      builder)
        self.assertIn('copy_as "${found}" "/usr/lib/x86_64-linux-gnu/${soname}"',
                      builder)

    def test_builder_refuses_a_non_x86_64_font_library(self) -> None:
        builder = BUILDER.read_text(encoding="utf-8")
        # The repository also builds a static FreeType for the iOS host.
        # Substituting it would satisfy a name check and leave Wine's Unix
        # side with nothing it can load.
        self.assertIn("is not an x86-64 ELF shared object", builder)

    def test_builder_proves_every_seeded_library_landed(self) -> None:
        builder = BUILDER.read_text(encoding="utf-8")
        self.assertIn("was not staged at", builder)


@unittest.skipUnless(shutil.which("bash") and shutil.which("unzip")
                     and shutil.which("od"),
                     "the packaging validator needs bash, unzip and od")
class FreeTypeArchiveValidation(WineserverArchiveValidation):
    """Drive the real validator against archives with and without FreeType.

    Subclassed only to reuse the archive helpers. The parent's own tests are
    detached below rather than inherited: re-running a two-and-a-half-minute
    suite a second time to borrow two helper methods is not a trade worth
    making, and a duplicate result is not a second piece of evidence.
    """

    def test_a_complete_font_backend_is_accepted(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(directory, server)
            code, output = self.run_validator(directory, glibc, wine)
            self.assertEqual(code, 0, output)
            for path in (FREETYPE_GLIBC_PATH, FREETYPE_WINE_PATH):
                self.assertIn(path + " is ELF64 EM_X86_64", output)

    def test_a_missing_font_library_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(
                directory, server, glibc_freetype=False, wine_freetype=False)
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0, output)
            self.assertIn("libfreetype.so.6", output)

    def test_a_font_library_in_only_one_layer_is_rejected(self) -> None:
        # The shape a single staged copy produces. The guest archive has no
        # symlink to make the other path resolve, so each layer needs its own.
        for glibc_has, wine_has, expected in (
                (True, False, FREETYPE_WINE_PATH),
                (False, True, FREETYPE_GLIBC_PATH)):
            with self.subTest(glibc=glibc_has, wine=wine_has):
                with tempfile.TemporaryDirectory() as raw:
                    directory = Path(raw)
                    server = elf(body=b"wineserver64 payload")
                    glibc, wine, _ = self.build_archives(
                        directory, server,
                        glibc_freetype=glibc_has, wine_freetype=wine_has)
                    code, output = self.run_validator(directory, glibc, wine)
                    self.assertNotEqual(code, 0, output)
                    self.assertIn(expected, output)

    def test_a_non_x86_64_font_library_is_rejected(self) -> None:
        # An ARM64 ELF passes every name check and cannot be loaded by the
        # guest at all.
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(
                directory, server, freetype=elf(machine=183))
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0, output)
            self.assertIn("EM_X86_64", output)

    def test_a_static_archive_masquerading_as_the_library_is_rejected(self) -> None:
        # What copying the iOS FreeType build in would look like from here.
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(
                directory, server, freetype=b"!<arch>\n" + b"\0" * 64)
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0, output)
            self.assertIn("is not an ELF file", output)


# Detach the inherited cases; see the class docstring. Assigning None leaves
# the attribute present but not callable, which is what unittest's collector
# filters on, so the helpers survive and the duplicate tests do not.
for _inherited in list(vars(WineserverArchiveValidation)):
    if _inherited.startswith("test_"):
        setattr(FreeTypeArchiveValidation, _inherited, None)


@unittest.skipUnless(shutil.which("bash") and shutil.which("unzip")
                     and shutil.which("od"),
                     "the packaging validator needs bash, unzip and od")
class WindowDriverPackaging(WineserverArchiveValidation):
    """The runtime has to contain everything a window needs to exist.

    A device run reached CreateWindowExW and got ERROR_INVALID_WINDOW_HANDLE
    with no X11 activity anywhere in the log. A process whose user driver never
    loaded has no desktop window to parent to, and Wine does not report that as
    an error of its own -- so the packaging is what has to be proven.
    """

    def test_a_complete_window_runtime_is_accepted(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(directory, server)
            code, output = self.run_validator(directory, glibc, wine)
            self.assertEqual(code, 0, output)
            self.assertIn("winex11.drv", output)
            self.assertIn("winex11.so", output)

    def test_a_missing_x11_user_driver_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(
                directory, server, x11_driver=False)
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0, output)
            self.assertIn("winex11", output)

    def test_a_missing_fontconfig_configuration_is_rejected(self) -> None:
        # Fontconfig loading with no default config is the exact shape a
        # device run produced: the library opens, and then every font lookup
        # fails.
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(directory, server)
            self.strip_from_archive(glibc, FONTCONFIG_PATH)
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0, output)
            self.assertIn("fonts.conf", output)

    def test_missing_x11_client_libraries_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(directory, server)
            self.strip_from_archive(glibc, "lib/x86_64-linux-gnu/libX11.so.6")
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0, output)
            self.assertIn("libX11.so.6", output)

    def test_missing_wine_bitmap_fonts_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            server = elf(body=b"wineserver64 payload")
            glibc, wine, _ = self.build_archives(directory, server)
            self.strip_from_archive(wine, DATA_ROOT + "/fonts/vgasys.fon")
            code, output = self.run_validator(directory, glibc, wine)
            self.assertNotEqual(code, 0, output)
            self.assertIn("vgasys.fon", output)

    @staticmethod
    def strip_from_archive(path: Path, drop: str) -> None:
        """Rewrite an archive without one entry, keeping the manifest valid."""
        with zipfile.ZipFile(path) as source:
            kept = [(item, source.read(item.filename))
                    for item in source.infolist()
                    if item.filename != drop]
        with zipfile.ZipFile(path, "w") as target:
            for item, payload in kept:
                target.writestr(item, payload)


for _inherited in list(vars(WineserverArchiveValidation)):
    if _inherited.startswith("test_"):
        setattr(WindowDriverPackaging, _inherited, None)
