#pragma once
// One-shot GPU oracle for channel order and gamma. Run only in the cube probe.
// Read pixels back from an offscreen target so presentation is a separate test.
inline void probeD3D9Colors(IDirect3DDevice9* device) {
    IDirect3DSurface9 *oldTarget = nullptr, *oldDepth = nullptr;
    IDirect3DSurface9 *target = nullptr, *readback = nullptr;
    IDirect3DStateBlock9* state = nullptr;
    IDirect3DTexture9* texture = nullptr;
    D3DVIEWPORT9 viewport{};
    if (FAILED(device->GetRenderTarget(0, &oldTarget))) return;
    device->GetDepthStencilSurface(&oldDepth);
    device->GetViewport(&viewport);
    HRESULT status = device->CreateStateBlock(D3DSBT_ALL, &state);
    if (SUCCEEDED(status)) status = state->Capture();
    if (SUCCEEDED(status)) status = device->CreateRenderTarget(16,16,D3DFMT_A8R8G8B8,
        D3DMULTISAMPLE_NONE,0,FALSE,&target,nullptr);
    if (SUCCEEDED(status)) status = device->CreateOffscreenPlainSurface(16,16,
        D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&readback,nullptr);
    auto report = [](const char* phase, HRESULT hr, DWORD actual, DWORD expected) {
        char line[256];
        const int length = std::snprintf(line,sizeof(line),
            "BOXEDWINE_D3D9_COLOR phase=%s hr=0x%08lx actual=0x%08lx expected=0x%08lx\n",
            phase,(unsigned long)hr,(unsigned long)actual,(unsigned long)expected);
        if (length > 0 && length < int(sizeof(line))) {
            DWORD written;
            WriteFile(GetStdHandle(STD_ERROR_HANDLE),line,DWORD(length),&written,nullptr);
        }
    };
    if (SUCCEEDED(status)) {
        device->SetDepthStencilSurface(nullptr);
        status = device->SetRenderTarget(0,target);
    }
    if (SUCCEEDED(status)) {
        device->SetRenderState(D3DRS_ZENABLE,FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE);
        device->SetRenderState(D3DRS_LIGHTING,FALSE);
        device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE,15);
        device->SetRenderState(D3DRS_SRGBWRITEENABLE,FALSE);
        device->SetTexture(0,nullptr);
        device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1);
        device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_DIFFUSE);
        device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1);
        device->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_DIFFUSE);
        device->SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_DISABLE);
        device->SetFVF(D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_TEX1);
        auto sample = [&](const char* phase, HRESULT hr, DWORD expected) {
            DWORD pixel = 0;
            if (SUCCEEDED(hr)) hr = device->GetRenderTargetData(target,readback);
            D3DLOCKED_RECT lock{};
            if (SUCCEEDED(hr)) {
                hr = readback->LockRect(&lock,nullptr,D3DLOCK_READONLY);
                if (SUCCEEDED(hr)) {
                    std::memcpy(&pixel,static_cast<char*>(lock.pBits)+8*lock.Pitch+8*4,4);
                    readback->UnlockRect();
                }
            }
            report(phase,hr,pixel,expected);
        };
        const DWORD colors[] = {0xffff0000u,0xff00ff00u,0xff0000ffu};
        const char* names[] = {"clear-red","clear-green","clear-blue"};
        for (unsigned i=0;i<3;++i)
            sample(names[i],device->Clear(0,nullptr,D3DCLEAR_TARGET,colors[i],1,0),colors[i]);
        struct ColorVertex { float x,y,z,w; DWORD color; float u,v; };
        auto draw = [&](DWORD color) {
            const ColorVertex vertices[] = {
                {-0.5f,-0.5f,0,1,color,0,0},{15.5f,-0.5f,0,1,color,1,0},
                {-0.5f,15.5f,0,1,color,0,1},{15.5f,15.5f,0,1,color,1,1}};
            HRESULT hr = device->BeginScene();
            if (SUCCEEDED(hr)) {
                hr = device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,vertices,sizeof(ColorVertex));
                const HRESULT end = device->EndScene();
                if (SUCCEEDED(hr)) hr = end;
            }
            return hr;
        };
        sample("vertex-red",draw(0xffff0000u),0xffff0000u);
        status = device->CreateTexture(1,1,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&texture,nullptr);
        D3DLOCKED_RECT lock{};
        if (SUCCEEDED(status)) status = texture->LockRect(0,&lock,nullptr,0);
        if (SUCCEEDED(status)) {
            const DWORD pixel=0xffff0000u;
            std::memcpy(lock.pBits,&pixel,4);
            texture->UnlockRect(0);
            device->SetTexture(0,texture);
            device->SetSamplerState(0,D3DSAMP_SRGBTEXTURE,FALSE);
            device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TEXTURE);
            sample("texture-red",draw(0xffffffffu),pixel);
            device->SetTexture(0,nullptr);
            device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_DIFFUSE);
        } else report("texture-setup",status,0,0);
        device->SetRenderState(D3DRS_SRGBWRITEENABLE,TRUE);
        sample("srgb-gray",draw(0xff808080u),0xffbcbcbcu); // +/-1 conversion tolerance
        device->SetRenderState(D3DRS_SRGBWRITEENABLE,FALSE);

        // Separate programmable constant/swizzle lowering from texture formats.
        // ps_2_0: mov oC0,c0 (then c0.bgra). No external shader compiler needed.
        const DWORD shaderIdentity[] = {0xffff0200u,0x02000001u,0x800f0800u,0xa0e40000u,0x0000ffffu};
        const DWORD shaderSwizzle[] = {0xffff0200u,0x02000001u,0x800f0800u,0xa0c60000u,0x0000ffffu};
        const float red[] = {1,0,0,1};
        device->SetPixelShaderConstantF(0,red,1);
        for (unsigned i=0;i<2;++i) {
            IDirect3DPixelShader9* shader = nullptr;
            HRESULT hr = device->CreatePixelShader(i ? shaderSwizzle : shaderIdentity,&shader);
            if (SUCCEEDED(hr)) hr = device->SetPixelShader(shader);
            if (SUCCEEDED(hr)) hr = draw(0xffffffffu);
            sample(i ? "shader-swizzle-blue" : "shader-constant-red",hr,i ? 0xff0000ffu : 0xffff0000u);
            device->SetPixelShader(nullptr);
            if (shader) shader->Release();
        }

        // Solid red BC blocks exercise the compressed upload and sampler path.
        // RGB565 endpoint 0xf800 is red; every selector chooses endpoint zero.
        const D3DFORMAT formats[] = {D3DFMT_A8B8G8R8,D3DFMT_DXT1,D3DFMT_DXT3,D3DFMT_DXT5};
        const char* phases[] = {"texture-rgba-red","texture-bc1-red","texture-bc2-red","texture-bc3-red"};
        for (unsigned i=0;i<4;++i) {
            IDirect3DTexture9* source = nullptr;
            HRESULT hr = device->CreateTexture(4,4,1,0,formats[i],D3DPOOL_MANAGED,&source,nullptr);
            D3DLOCKED_RECT upload{};
            if (SUCCEEDED(hr)) hr = source->LockRect(0,&upload,nullptr,0);
            if (SUCCEEDED(hr)) {
                if (i==0) {
                    const DWORD rgbaRed=0xff0000ffu;
                    for (unsigned y=0;y<4;++y) for (unsigned x=0;x<4;++x)
                        std::memcpy(static_cast<char*>(upload.pBits)+y*upload.Pitch+x*4,&rgbaRed,4);
                } else {
                    unsigned char block[16]{};
                    const unsigned offset=i==1 ? 0 : 8;
                    block[offset+1]=0xf8;
                    if (i==2) std::memset(block,0xff,8); // BC2 opaque alpha
                    if (i==3) block[0]=0xff; // BC3 alpha endpoint zero = 1
                    std::memcpy(upload.pBits,block,i==1 ? 8 : 16);
                }
                hr=source->UnlockRect(0);
            }
            if (SUCCEEDED(hr)) hr=device->SetTexture(0,source);
            device->SetSamplerState(0,D3DSAMP_SRGBTEXTURE,FALSE);
            device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TEXTURE);
            if (SUCCEEDED(hr)) hr=draw(0xffffffffu);
            sample(phases[i],hr,0xffff0000u);
            device->SetTexture(0,nullptr);
            if (source) source->Release();
        }
    } else report("setup",status,0,0);
    if (state) {state->Apply();state->Release();}
    device->SetRenderTarget(0,oldTarget);
    device->SetDepthStencilSurface(oldDepth);
    device->SetViewport(&viewport);
    if (texture) texture->Release();
    if (readback) readback->Release();
    if (target) target->Release();
    if (oldDepth) oldDepth->Release();
    oldTarget->Release();
}
