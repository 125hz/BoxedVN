"""Run the production category setter across actual SDL teardown/restart."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--sdl-root", type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
text = (root / "platform/sdl/kdspaudio.cpp").read_text()
start = text.index("static void ensureAudioSessionCategory() {")
function = text[start:text.index("\n}", start) + 2]
fixture = r'''
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <cassert>
#include <cstring>
FUNCTION
int main() {
    SDL_ClearHints();
    for (int session=0; session<3; session++) {
        assert(SDL_Init(0)==0);
        ensureAudioSessionCategory();
        auto hint = SDL_GetHint(SDL_HINT_AUDIO_CATEGORY);
        assert(hint && std::strcmp(hint,"playback")==0);
        SDL_Quit();
        assert(!SDL_GetHint(SDL_HINT_AUDIO_CATEGORY));
    }
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_CATEGORY,"ambient",SDL_HINT_OVERRIDE);
    ensureAudioSessionCategory();
    assert(std::strcmp(SDL_GetHint(SDL_HINT_AUDIO_CATEGORY),"ambient")==0);
    SDL_ClearHints();
}
'''.replace("FUNCTION", function)
with tempfile.TemporaryDirectory(prefix="boxedvn-audio-category-") as temp:
    cpp = Path(temp) / "category.cpp"
    cpp.write_text(fixture)
    exe = Path(temp) / ("category.exe" if os.name == "nt" else "category")
    env = os.environ.copy()
    env.pop("SDL_AUDIO_CATEGORY", None)
    if os.name == "nt":
        assert args.sdl_root, "--sdl-root required on Windows"
        sdl = args.sdl_root.resolve()
        cmd = ["cl", "/nologo", "/EHsc", "/std:c++17", str(cpp),
               f"/I{sdl / 'include'}", f"/Fe{exe}", str(sdl / "lib/x64/SDL2.lib")]
        env["PATH"] = str(sdl / "lib/x64") + os.pathsep + env["PATH"]
    else:
        flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "sdl2"], text=True))
        cmd = ["c++", "-std=c++17", str(cpp), "-o", str(exe), *flags]
    subprocess.run(cmd, cwd=temp, env=env, check=True)
    subprocess.run([str(exe)], cwd=temp, env=env, check=True)
print("SDL audio category: three sessions and explicit override passed")
