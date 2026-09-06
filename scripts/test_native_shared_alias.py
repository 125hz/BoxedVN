"""Run the production shared-map path against real Darwin VM aliases."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "source/kernel/kmemory64.cpp").read_text(encoding="utf-8")
registry = source[source.index("namespace {\nstruct SharedFilePage"):
                  source.index("// ---- Guest page-flag provenance")]
body = source[source.index("U64 KMemory64::mmapSharedFile("):
              source.index("// Record a reservation in the ordered")]
fixture = (root / "ios/tests/test_native_shared_alias.cpp.in").read_text(encoding="utf-8")
fixture = fixture.replace("@REGISTRY@", registry).replace("@MAP_BODY@", body)
with tempfile.TemporaryDirectory(prefix="boxedvn-shared-alias-") as tmp:
    cpp, exe = Path(tmp) / "test.cpp", Path(tmp) / "test"
    cpp.write_text(fixture, encoding="utf-8")
    for granule in [None, 16384]:
        defines=[] if granule is None else [f"-DBOXEDVN_NATIVE_ALIAS_TEST_GRANULE={granule}"]
        subprocess.run(["xcrun", "clang++", "-std=c++17", "-Wall", "-Wextra", *defines,
                        "-I" + str(root / "include"), str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
