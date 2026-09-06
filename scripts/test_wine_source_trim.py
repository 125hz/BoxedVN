"""Exercise source-runtime staging against a real ELF, including path safety."""
from pathlib import Path
import ctypes
import os
import re
import shutil
import subprocess
import tempfile

repo = Path(__file__).resolve().parent.parent
source = (repo / "scripts/build-wine64-runtime-ci.sh").read_text()
def function(name):
    match = re.search(r"^" + name + r"\(\) \{.*?^\}", source, re.M | re.S)
    assert match, name
    return match.group(0)
script = "set -euo pipefail\ndie() { echo \"$*\" >&2; exit 1; }\nrequire_command() { command -v \"$1\" >/dev/null; }\n"
script += function("is_elf64_x86_64") + "\n" + function("trim_source_runtime_tree")
script += '\ntrim_source_runtime_tree "$1" strip\n'
with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    stage = root / "stage"
    stage.mkdir()
    code = root / "probe.c"
    code.write_text("int runtime_value(void) { return 137; }\n")
    original = root / "original.so"
    subprocess.run(["gcc", "-g", "-shared", "-fPIC", str(code), "-o", str(original)], check=True)
    module = stage / "runtime.so"
    shutil.copy2(original, module)
    (stage / "developer.a").write_bytes(b"developer import archive")
    (stage / "metadata.txt").write_text("keep this resource")
    env = dict(os.environ, WINE_INSTALL="source-install", STAGE=str(stage), PE32_STAGE=str(root / "pe32"))
    subprocess.run(["bash", "-c", script, "trim", str(stage / ".") + "/"], env=env, check=True)
    assert original.read_bytes() != module.read_bytes()
    assert b".debug_info" in original.read_bytes() and b".debug_info" not in module.read_bytes()
    assert ctypes.CDLL(str(module)).runtime_value() == 137
    assert not (stage / "developer.a").exists()
    assert (stage / "metadata.txt").read_text() == "keep this resource"
    before = original.read_bytes()
    denied = subprocess.run(["bash", "-c", script, "trim", str(root)], env=env, capture_output=True)
    assert denied.returncode != 0 and original.read_bytes() == before
print("Runtime trimming removes debug/developer data, preserves exports, and rejects source paths")
