"""Link the real raw-event codec and verify the internal 32-bit wire ABI."""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory() as temporary:
    source = Path(temporary) / "event.cpp"
    source.write_text(r"""
#include "boxedwine.h"
#include "x11/x11.h"
#include <cassert>
int main() {
    const U32 wire[15] = {35, 91, 1, 0x1234, 131, 17, 123456,
                         2, 2, 0, 0, 1, 0xfffffff0, 0x7fffffff, 0};
    XIRawEvent event = {};
    event.unserialize(wire);
    assert(event.type == GenericEvent && event.evtype == XI_RawMotion);
    assert(event.serial == 91 && event.time == 123456 && event.deviceid == 2);
    assert((S32)event.valuators.maskAddress == -16);
    assert((S32)event.valuators.valuesAddress == 2147483647);
    U32 output[15] = {};
    event.serialize(output);
    for (unsigned i = 0; i < 15; ++i) assert(output[i] == wire[i]);
}
""")
    executable = Path(temporary) / "event"
    flags = ["-std=c++20", "-ffunction-sections", "-fdata-sections",
             "-DSIMDE_NO_NATIVE", "-DBOXEDWINE_64", "-DBOXEDWINE_POSIX",
             "-DBOXEDWINE_MULTI_THREADED", "-DSDL2=1"]
    for directory in ["include", "lib/simde", "source", "lib/sdl2/include"]:
        flags += ["-I" + str(repo / directory)]
    subprocess.run(["g++", *flags, str(source), str(repo / "source/x11/xinput2.cpp"),
                    "-Wl,--gc-sections", "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
print("Production raw-event codec links and preserves signed deltas and metadata")
