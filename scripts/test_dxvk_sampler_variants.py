"""Compile the patched DXSO binding helpers and check the entire D3D9 layout."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("source", type=Path)
args = parser.parse_args()
source = args.source.resolve()
util = (source / "src/dxso/dxso_util.h").read_text()
helpers = util[util.index("  constexpr uint32_t SamplerVariantCount"):util.index("  uint32_t RegisterLinkerSlot")]
constants = util[util.index("  enum DxsoConstantBuffers"):util.index("  // Separate image variants")]
caps = (source / "src/d3d9/d3d9_caps.h").read_text()
counts = {name: int(re.search(rf"{name}\s*=\s*(\d+)", caps)[1]) for name in ("MaxTexturesVS", "MaxTexturesPS")}
limits = (source / "src/dxvk/dxvk_limits.h").read_text()
limit = int(re.search(r"MaxNumResourceSlots\s*=\s*(\d+)", limits)[1])
fixture = r'''
#include <cstdint>
#include <set>
#include <cassert>
enum D3DRESOURCETYPE { D3DRTYPE_TEXTURE=3, D3DRTYPE_VOLUMETEXTURE=4, D3DRTYPE_CUBETEXTURE=5 };
enum class DxsoProgramType { VertexShader=0, PixelShader=1 };
enum class DxsoBindingType { ConstantBuffer, Image };
namespace caps { constexpr uint32_t MaxTexturesVS=VS_TEXTURES, MaxTexturesPS=PS_TEXTURES; }
CONSTANTS
HELPERS
int main() {
    std::set<uint32_t> slots;
    for (uint32_t stage=0; stage<2; stage++) {
        auto shader = DxsoProgramType(stage);
        for (uint32_t cb=0; cb<(stage ? PSCount : VSCount); cb++)
            assert(slots.insert(computeResourceSlotId(shader, DxsoBindingType::ConstantBuffer, cb)).second);
        for (uint32_t texture=0; texture<(stage ? caps::MaxTexturesPS : caps::MaxTexturesVS); texture++) {
            auto base = computeResourceSlotId(shader, DxsoBindingType::Image, texture);
            std::set<uint32_t> variants;
            for (uint32_t type=0; type<3; type++) {
                assert(variants.insert(samplerTypeVariant(type, false)).second);
                if (type!=1) assert(variants.insert(samplerTypeVariant(type, true)).second);
            }
            assert(variants.size()==5);
            for (auto v : variants) assert(slots.insert(base+v).second);
        }
    }
    assert(slots.insert(getSWVPBufferSlot()).second);
    assert(slots.insert(getSpecConstantBufferSlot()).second);
    assert(*slots.rbegin() < RESOURCE_LIMIT);
    assert(textureTypeVariant(D3DRTYPE_TEXTURE,false)==0);
    assert(textureTypeVariant(D3DRTYPE_VOLUMETEXTURE,false)==1);
    assert(textureTypeVariant(D3DRTYPE_CUBETEXTURE,false)==2);
    assert(textureTypeVariant(D3DRTYPE_TEXTURE,true)==3);
    assert(textureTypeVariant(D3DRTYPE_CUBETEXTURE,true)==4);
}
'''
for key, value in {"VS_TEXTURES": counts["MaxTexturesVS"], "PS_TEXTURES": counts["MaxTexturesPS"],
                   "CONSTANTS": constants, "HELPERS": helpers, "RESOURCE_LIMIT": limit}.items():
    fixture = fixture.replace(key, str(value))
with tempfile.TemporaryDirectory(prefix="boxedvn-sampler-") as temp:
    cpp = Path(temp) / "samplers.cpp"
    cpp.write_text(fixture)
    exe = Path(temp) / ("samplers.exe" if os.name == "nt" else "samplers")
    if os.name == "nt":
        command = ["cl", "/nologo", "/EHsc", "/std:c++17", str(cpp), f"/Fe{exe}"]
    else:
        command = ["c++", "-std=c++17", str(cpp), "-o", str(exe)]
    subprocess.run(command, cwd=temp, check=True)
    subprocess.run([str(exe)], check=True)
print("D3D9: all stage/texture/variant/constant slots are distinct and in range")
