"""Validate rebuilt PE32 DXVK modules and the worker diagnostic payload."""
import struct
import sys
from pathlib import Path

root = Path(sys.argv[1])
for module in ("d3d9", "d3d11", "dxgi", "d3d10core"):
    data = (root / f"{module}.dll").read_bytes()
    assert data[:2] == b"MZ", module
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[pe:pe + 4] == b"PE\0\0", module
    assert struct.unpack_from("<H", data, pe + 4)[0] == 0x14C, module
    assert struct.unpack_from("<H", data, pe + 24)[0] == 0x10B, module
    if module in ("d3d9", "d3d11"):
        assert b"BOXEDWINE_DXVK_WORKER" in data, f"{module}: missing worker report"
        assert b"BOXEDWINE_DXVK_ALLOCATION_FAILED" in data, f"{module}: missing allocation report"
        assert b"new_aligned" in data, f"{module}: missing aligned allocation report"
        assert b"allocator_reject_size_unknown" in data, f"{module}: missing allocator rejection report"
        assert b"non-std exception escaped worker" in data, module
    print(f"{module}.dll: PE32 i386 validated")
assert (root / "build-manifest.txt").is_file(), "missing source provenance"
