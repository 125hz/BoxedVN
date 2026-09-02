// BoxedVN - shaders for the x86-64 Direct3D 11 acceptance probe.
// Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
//
// Compiled ahead of time with fxc (shader model 4.0) into
// x64-d3d11-cube-shaders.h by scripts/generate-x64-probe-shaders.py. The
// probe embeds the DXBC so it needs no HLSL compiler inside the guest; the
// translation under test is DXBC to Metal, which DXMT performs at pipeline
// creation.
//
// The matrix is row_major so the C side can upload a plain row-major array
// and mul(mvp, v) applies it as M * v without a transpose on either side.

cbuffer Constants : register(b0)
{
    row_major float4x4 mvp;
};

struct VSInput
{
    float3 position : POSITION;
    float3 colour   : COLOR;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 colour   : COLOR;
};

VSOutput vs_main(VSInput input)
{
    VSOutput output;
    output.position = mul(mvp, float4(input.position, 1.0));
    output.colour = input.colour;
    return output;
}

float4 ps_main(VSOutput input) : SV_Target
{
    return float4(input.colour, 1.0);
}
