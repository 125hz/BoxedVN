// BoxedVN - shaders for the x86-64 Direct3D 11 acceptance probe.
//
// PORTED VERBATIM from DXMT's own Direct3D 11 cube test, which is the demo
// the sibling iOS Wine/FEX/DXMT project (Madeira) builds and runs on device:
//
//   source: https://github.com/willfaust/dxmt/blob/b4b89f0a5a1752da3982a7b6c5575506024bf253/tests/dx11/shader_cube.hlsl
//   commit: b4b89f0a5a1752da3982a7b6c5575506024bf253 (branch ios-port)
//   used by: https://github.com/willfaust/Madeira/blob/main/build/dxmt-tests/build-x64.sh
//
// DXMT is MIT licensed:
//
//   MIT License
//   Copyright (c) 2023 Feifan He
//   Permission is hereby granted, free of charge, to any person obtaining a
//   copy of this software and associated documentation files (the
//   "Software"), to deal in the Software without restriction, including
//   without limitation the rights to use, copy, modify, merge, publish,
//   distribute, sublicense, and/or sell copies of the Software, and to permit
//   persons to whom the Software is furnished to do so, subject to the
//   following conditions:
//   The above copyright notice and this permission notice shall be included
//   in all copies or substantial portions of the Software.
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
//   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
//   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
//   NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
//   DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
//   OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
//   USE OR OTHER DEALINGS IN THE SOFTWARE.
//
// Only these comments are added. The matrix is left at HLSL's default
// (column-major) packing and multiplied as mul(v, M) with v a row vector,
// which is what the C side's float m[4][4] uploads unchanged: that C layout
// stores column j at m[j][0..3], which is exactly one constant register per
// column. Changing either side alone renders nothing recognisable.
//
// Compiled ahead of time with fxc into x64-d3d11-cube-shaders.h by
// scripts/generate-x64-probe-shaders.py, so the probe needs no HLSL compiler
// inside the guest; the translation under test is DXBC to Metal, which DXMT
// performs at pipeline creation.

cbuffer constants : register(b0)
{
    float4x4 modelViewProj;
};

struct VS_Input {
    float3 pos : POS;
};

struct VS_Output {
    float4 pos : SV_POSITION;
    float3 color : COLOR;
};

VS_Output vs_main(VS_Input input)
{
    VS_Output output;
    output.pos = mul(float4(input.pos, 1.0f), modelViewProj);
    // This is just a dumb bit of maths to color our unit cube nicely
    output.color = input.pos + float3(0.5f, 0.5f, 0.5f);
    return output;
}

float4 ps_main(VS_Output input) : SV_Target
{
    return float4(abs(input.color), 1.0);
}
