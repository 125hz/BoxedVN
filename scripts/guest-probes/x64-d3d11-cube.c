/*
 * BoxedVN - x86-64 Direct3D 11 acceptance probe.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 *
 * WHY THIS EXISTS
 *
 * The staged probe used to be an opaque prebuilt executable. Two device runs
 * showed it initialising Wine successfully, finding both DXMT DLLs, executing
 * about fourteen thousand guest syscalls, and then leaving through
 * exit_group(231) with status 1400 -- and 1400 could have come from any of
 * three branches, because each one displays a message box before reading
 * GetLastError, and a message box overwrites the thread's last error. Nothing
 * in the log said which call actually failed.
 *
 * So this probe states its progress. Every stage announces itself before the
 * call and reports the outcome immediately after, capturing GetLastError and
 * the HRESULT at the point of failure, before anything else can overwrite
 * them. The markers go to the Windows stderr handle, which BoxedVN already
 * captures as `[guest fd=2 ...]`.
 *
 * WHAT IT DRAWS
 *
 * DXMT's own Direct3D 11 cube test -- the demo the sibling iOS
 * Wine/FEX/DXMT project (Madeira) builds and runs on device -- ported into
 * this probe's instrumentation:
 *
 *   renderer:  https://github.com/willfaust/dxmt/blob/b4b89f0a5a1752da3982a7b6c5575506024bf253/tests/dx11/dx11_cube.cpp
 *   maths:     https://github.com/willfaust/dxmt/blob/b4b89f0a5a1752da3982a7b6c5575506024bf253/tests/dx11/3DMaths.h
 *   shader:    https://github.com/willfaust/dxmt/blob/b4b89f0a5a1752da3982a7b6c5575506024bf253/tests/dx11/shader_cube.hlsl
 *   commit:    b4b89f0a5a1752da3982a7b6c5575506024bf253 (branch ios-port)
 *   consumer:  https://github.com/willfaust/Madeira/blob/main/build/dxmt-tests/build-x64.sh
 *
 * DXMT is MIT licensed, Copyright (c) 2023 Feifan He; the notice is kept in
 * full beside the ported geometry and matrices below and in
 * THIRD_PARTY_NOTICES.md. Its cube test in turn follows Kevin Moran's
 * BeginnerDirect3D11 tutorial ("08. Drawing a Cube"), which is where the
 * matrix helpers and the camera come from.
 *
 * What is ported: the eight position-only vertices and their 36 indices, the
 * rasterizer and depth-stencil state, the column-major matrix helpers and the
 * projection, the camera at (0, 0, 2), the clear colour, the dynamic constant
 * buffer written through Map/Unmap, and the wall-clock spin. What is NOT
 * ported: the demo's WinMain shell, its keyboard camera, its mouse trace and
 * its bitmap-font overlay, its 1024x768 hardcoding, and its
 * D3DCompileFromFile call -- the shaders are compiled ahead of time
 * (x64-d3d11-cube.hlsl, embedded via x64-d3d11-cube-shaders.h) so no HLSL
 * compiler runs inside the guest, which is what Madeira's own build does with
 * its test_shim.h.
 *
 * In its place this file keeps the launcher contract: a console image, the
 * stage markers below, the per-stage exit codes, and the window creation the
 * device logs are read against.
 *
 * The demo draws with a depth buffer, so this does too; if the depth
 * resources cannot be created the loop reports it and carries on without
 * them, because the cube is convex and back-face culling alone renders it
 * correctly. That keeps a depth-buffer defect from ending a run that would
 * otherwise say something about the draw.
 *
 * DIAGNOSTIC DISCIPLINE
 *
 * A device log has to stay readable, so: one line per stage, first occurrence
 * only, and nothing per frame. The presentation loop announces its FIRST
 * present and then goes quiet for good.
 *
 * The exit code names the stage that failed and is fixed per stage, so the
 * process status is meaningful on its own even if stderr is lost. The original
 * Win32 error and HRESULT are preserved in the marker rather than returned:
 * a process status has room for one number, and the marker does not.
 *
 * Built as a CONSOLE subsystem image on purpose. A -mwindows build has no
 * stderr handle to write to, and the markers are the entire reason this
 * program exists.
 */

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <math.h>
#include <stdio.h>

#include "x64-d3d11-cube-shaders.h"

/* Deterministic per-stage exit codes. Documented here because the process
 * status may be the only thing that survives. */
enum {
    BOXEDVN_EXIT_OK = 0,
    BOXEDVN_EXIT_REGISTER_CLASS = 10,
    BOXEDVN_EXIT_CREATE_WINDOW = 11,
    BOXEDVN_EXIT_D3D11_CREATE = 12,
    BOXEDVN_EXIT_DXGI_FACTORY = 13,
    BOXEDVN_EXIT_SWAPCHAIN = 14,
    BOXEDVN_EXIT_RENDER_TARGET = 15,
    BOXEDVN_EXIT_PRESENT = 16,
    BOXEDVN_EXIT_SHADERS = 17,
    BOXEDVN_EXIT_GEOMETRY = 18
};

static const char kStagePrefix[] = "BOXEDVN_X64_CUBE_STAGE ";

/* The acceptance run is the first 240 frames; after that the probe keeps
 * drawing until its window is closed, so it can be watched and measured. */
enum { kProbeWidth = 640, kProbeHeight = 480, kProbeAcceptanceFrames = 240 };

/* Written through the Windows stderr handle rather than the CRT, so the bytes
 * reach the guest's fd 2 in one unbuffered write that cannot be lost if the
 * process dies immediately afterwards. */
static void stage_write(const char* text) {
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    DWORD written = 0;
    DWORD length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    if (err != NULL && err != INVALID_HANDLE_VALUE) {
        WriteFile(err, text, length, &written, NULL);
    }
}

static void stage_line(const char* body) {
    char line[512];
    snprintf(line, sizeof(line), "%s%s\n", kStagePrefix, body);
    stage_write(line);
}

static void stage_begin(const char* stage) {
    char body[256];
    snprintf(body, sizeof(body), "%s begin", stage);
    stage_line(body);
}

static void stage_ok(const char* stage, const char* detail) {
    char body[320];
    if (detail != NULL && detail[0] != '\0') {
        snprintf(body, sizeof(body), "%s ok %s", stage, detail);
    } else {
        snprintf(body, sizeof(body), "%s ok", stage);
    }
    stage_line(body);
}

/* `win32` is captured by the caller at the failing call. Passing it in rather
 * than reading GetLastError here is the whole point: by the time a reporting
 * helper runs, any intervening call may already have replaced it. */
static void stage_fail(const char* stage, DWORD win32, const char* extra) {
    char body[384];
    if (extra != NULL && extra[0] != '\0') {
        snprintf(body, sizeof(body), "%s fail %s win32=%lu", stage, extra,
                 (unsigned long)win32);
    } else {
        snprintf(body, sizeof(body), "%s fail win32=%lu", stage,
                 (unsigned long)win32);
    }
    stage_line(body);
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam,
                                    LPARAM lparam) {
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

/* ------------------------------------------------------------------------ */
/* Geometry, matrices and camera, PORTED from DXMT's Direct3D 11 cube test
 * and its 3DMaths.h (see the file header for the URLs and the commit).
 *
 *   MIT License
 *   Copyright (c) 2023 Feifan He
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a
 *   copy of this software and associated documentation files (the
 *   "Software"), to deal in the Software without restriction, including
 *   without limitation the rights to use, copy, modify, merge, publish,
 *   distribute, sublicense, and/or sell copies of the Software, and to
 *   permit persons to whom the Software is furnished to do so, subject to
 *   the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included
 *   in all copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 *   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 *   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 *   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 *   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 *   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 *   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The C++ original uses operator overloads on a float4x4 union; this is the
 * same arithmetic written out in C11. Nothing about the values changed.
 *
 * The cube is position only: the pixel colour is derived from the position
 * in the vertex shader. Its 12 triangles are wound COUNTER-clockwise as seen
 * from outside, which is why the rasterizer state below sets
 * FrontCounterClockwise and culls back faces -- the demo's own state. */
struct probe_vertex {
    float position[3];
};

static const struct probe_vertex kCubeVertices[8] = {
    {{-0.5f, -0.5f, -0.5f}},
    {{-0.5f, -0.5f,  0.5f}},
    {{-0.5f,  0.5f, -0.5f}},
    {{-0.5f,  0.5f,  0.5f}},
    {{ 0.5f, -0.5f, -0.5f}},
    {{ 0.5f, -0.5f,  0.5f}},
    {{ 0.5f,  0.5f, -0.5f}},
    {{ 0.5f,  0.5f,  0.5f}},
};

static const WORD kCubeIndices[36] = {
    0, 6, 4,
    0, 2, 6,
    0, 3, 2,
    0, 1, 3,
    2, 7, 6,
    2, 3, 7,
    4, 6, 7,
    4, 7, 5,
    0, 4, 5,
    0, 5, 1,
    1, 5, 7,
    1, 7, 3,
};

/* 4x4 matrices stored the way the ported 3DMaths.h stores them: m[column]
 * is one constant register, so m[j][i] is the element in column j, row i.
 * Vectors are rows and are applied as v * M, which is what the HLSL does
 * with mul(float4(pos, 1), modelViewProj) at the default (column-major)
 * packing. The array is uploaded to the constant buffer unchanged. */
struct probe_matrix {
    float m[4][4];
};

/* dot(a.row(i), b.cols[j]) written out: the original's
 * `float4x4 operator*` in the same order. */
static struct probe_matrix matrix_multiply(const struct probe_matrix* a,
                                           const struct probe_matrix* b) {
    struct probe_matrix r;
    int i, j, k;
    for (j = 0; j < 4; ++j) {
        for (i = 0; i < 4; ++i) {
            float sum = 0.0f;
            for (k = 0; k < 4; ++k) {
                sum += a->m[k][i] * b->m[j][k];
            }
            r.m[j][i] = sum;
        }
    }
    return r;
}

static struct probe_matrix matrix_rotation_x(float rad) {
    const float s = sinf(rad), c = cosf(rad);
    struct probe_matrix r = {{
        {1.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, c,   -s,    0.0f},
        {0.0f, s,    c,    0.0f},
        {0.0f, 0.0f, 0.0f, 1.0f},
    }};
    return r;
}

static struct probe_matrix matrix_rotation_y(float rad) {
    const float s = sinf(rad), c = cosf(rad);
    struct probe_matrix r = {{
        {c,    0.0f, s,    0.0f},
        {0.0f, 1.0f, 0.0f, 0.0f},
        {-s,   0.0f, c,    0.0f},
        {0.0f, 0.0f, 0.0f, 1.0f},
    }};
    return r;
}

static struct probe_matrix matrix_translation(float x, float y, float z) {
    struct probe_matrix r = {{
        {1.0f, 0.0f, 0.0f, x},
        {0.0f, 1.0f, 0.0f, y},
        {0.0f, 0.0f, 1.0f, z},
        {0.0f, 0.0f, 0.0f, 1.0f},
    }};
    return r;
}

/* makePerspectiveMat: right-handed, looking down -Z, depth in [0, 1].
 * 1/tan(x) == tan(90deg - x), which is why there is no divide. */
static struct probe_matrix matrix_perspective(float aspect, float fov_y_rad,
                                              float near_z, float far_z) {
    const float y_scale = tanf(0.5f * (3.14159265358979323846f - fov_y_rad));
    const float x_scale = y_scale / aspect;
    const float z_range_inverse = 1.0f / (near_z - far_z);
    const float z_scale = far_z * z_range_inverse;
    const float z_translation = far_z * near_z * z_range_inverse;
    struct probe_matrix r = {{
        {x_scale, 0.0f,    0.0f,     0.0f},
        {0.0f,    y_scale, 0.0f,     0.0f},
        {0.0f,    0.0f,    z_scale,  z_translation},
        {0.0f,    0.0f,   -1.0f,     0.0f},
    }};
    return r;
}

static float degrees_to_radians(float degrees) {
    return degrees * (3.14159265358979323846f / 180.0f);
}

/* The spin is a function of ELAPSED WALL-CLOCK SECONDS, never of the frame
 * counter. With the host frame limiter unlocked the guest presents as fast as
 * it can -- a per-frame angle turned the cube into a blur at a few thousand
 * frames a second, and the same program looked slow at 30. These are the
 * demo's own rates: -0.2*pi rad/s about X, 0.1*pi rad/s about Y. */
static struct probe_matrix cube_transform(double seconds) {
    const float t = (float)seconds;
    struct probe_matrix rotate_x =
        matrix_rotation_x(-0.2f * 3.14159265358979323846f * t);
    struct probe_matrix rotate_y =
        matrix_rotation_y(0.1f * 3.14159265358979323846f * t);
    struct probe_matrix model = matrix_multiply(&rotate_x, &rotate_y);
    /* The demo's camera sits at (0, 0, 2) with no pitch or yaw, so its
     * view matrix reduces to a translation by -cameraPos. */
    struct probe_matrix view = matrix_translation(0.0f, 0.0f, -2.0f);
    struct probe_matrix projection = matrix_perspective(
        (float)kProbeWidth / (float)kProbeHeight, degrees_to_radians(84.0f),
        0.1f, 1000.0f);
    struct probe_matrix model_view = matrix_multiply(&model, &view);
    return matrix_multiply(&model_view, &projection);
}

int main(void) {
    static const WCHAR kClassName[] = L"BoxedVNX64CubeProbe";
    WNDCLASSEXW wc;
    HINSTANCE instance = GetModuleHandleW(NULL);
    ATOM atom;
    DWORD captured;
    HWND window;
    DXGI_SWAP_CHAIN_DESC swap_desc;
    IDXGISwapChain* swapchain = NULL;
    IDXGIFactory* factory = NULL;
    ID3D11Device* device = NULL;
    ID3D11DeviceContext* context = NULL;
    ID3D11Texture2D* backbuffer = NULL;
    ID3D11RenderTargetView* rtv = NULL;
    ID3D11Texture2D* depth_texture = NULL;
    ID3D11DepthStencilView* depth_view = NULL;
    ID3D11VertexShader* vertex_shader = NULL;
    ID3D11PixelShader* pixel_shader = NULL;
    ID3D11InputLayout* input_layout = NULL;
    ID3D11Buffer* vertex_buffer = NULL;
    ID3D11Buffer* index_buffer = NULL;
    ID3D11Buffer* constant_buffer = NULL;
    ID3D11RasterizerState* rasterizer_state = NULL;
    ID3D11DepthStencilState* depth_state = NULL;
    D3D_FEATURE_LEVEL level = (D3D_FEATURE_LEVEL)0;
    LARGE_INTEGER counter_frequency;
    LARGE_INTEGER counter_start;
    HRESULT hr;
    char detail[320];
    int frames;
    int presented = 0;
    int mapped_failed = 0;

    /* ---------------------------------------------------------------- */
    stage_begin("register-class");
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    /* IDC_ARROW expands to the ANSI form unless UNICODE is defined, and this
     * file calls the W entry points explicitly rather than relying on that
     * macro switch. Name the standard arrow cursor's resource id directly. */
    wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = kClassName;
    SetLastError(0);
    atom = RegisterClassExW(&wc);
    captured = GetLastError();
    if (atom == 0) {
        stage_fail("register-class", captured, NULL);
        return BOXEDVN_EXIT_REGISTER_CLASS;
    }
    stage_ok("register-class", NULL);

    /* ---------------------------------------------------------------- */
    /* What the window would be created INTO, reported before trying.
     *
     * A device run returned ERROR_INVALID_WINDOW_HANDLE from CreateWindowExW
     * with no failing handle of its own to blame -- the value points at
     * something the window would have been parented to or associated with.
     * A desktop window of zero means the user driver never initialised, which
     * is a different failure from one that has a desktop and still refuses. */
    {
        HWND desktop = GetDesktopWindow();
        HDESK threadDesktop = GetThreadDesktop(GetCurrentThreadId());
        HWINSTA station = GetProcessWindowStation();
        char display[128];
        DWORD displayLength =
            GetEnvironmentVariableA("DISPLAY", display, sizeof(display));
        if (displayLength == 0 || displayLength >= sizeof(display)) {
            display[0] = '\0';
        }
        snprintf(detail, sizeof(detail),
                 "desktop-env display='%s' desktop=0x%llx thread_desktop=0x%llx "
                 "window_station=0x%llx",
                 display[0] != '\0' ? display : "(unset)",
                 (unsigned long long)(ULONG_PTR)desktop,
                 (unsigned long long)(ULONG_PTR)threadDesktop,
                 (unsigned long long)(ULONG_PTR)station);
        stage_line(detail);
    }

    stage_begin("create-window");
    SetLastError(0);
    window = CreateWindowExW(0, kClassName, L"BoxedVN D3D11 probe",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             kProbeWidth, kProbeHeight, NULL, NULL, instance,
                             NULL);
    captured = GetLastError();
    if (window == NULL) {
        stage_fail("create-window", captured, NULL);
        /* One narrower attempt, to separate two very different failures: a
         * user driver that is not there at all, and one that is there but
         * cannot build a full overlapped frame -- the non-client area needs
         * fonts and metrics that a plain popup does not.
         *
         * Reported as its own result and never treated as success. The
         * program still exits on the create-window stage code, and no handle
         * from here is passed to DXGI: a window the driver could not build
         * properly is not a surface anything can present to. */
        stage_begin("create-window-popup");
        SetLastError(0);
        window = CreateWindowExW(0, kClassName, L"BoxedVN D3D11 probe",
                                 WS_POPUP, 0, 0, kProbeWidth, kProbeHeight,
                                 NULL, NULL, instance, NULL);
        captured = GetLastError();
        if (window == NULL) {
            stage_fail("create-window-popup", captured, NULL);
        } else {
            snprintf(detail, sizeof(detail), "hwnd=0x%llx",
                     (unsigned long long)(ULONG_PTR)window);
            stage_ok("create-window-popup", detail);
            DestroyWindow(window);
        }
        return BOXEDVN_EXIT_CREATE_WINDOW;
    }
    snprintf(detail, sizeof(detail), "hwnd=0x%llx",
             (unsigned long long)(ULONG_PTR)window);
    stage_ok("create-window", detail);
    ShowWindow(window, SW_SHOW);

    /* ---------------------------------------------------------------- */
    /* D3D11CreateDevice, not the AndSwapChain form: the device and the
     * swap chain fail for different reasons, and collapsing them would put
     * this iteration back where it started. */
    stage_begin("d3d11-create");
    SetLastError(0);
    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
                           D3D11_SDK_VERSION, &device, &level, &context);
    captured = GetLastError();
    if (FAILED(hr) || device == NULL) {
        snprintf(detail, sizeof(detail), "hr=0x%08lx", (unsigned long)hr);
        stage_fail("d3d11-create", captured, detail);
        return BOXEDVN_EXIT_D3D11_CREATE;
    }
    snprintf(detail, sizeof(detail), "hr=0x%08lx feature_level=0x%04x",
             (unsigned long)hr, (unsigned)level);
    stage_ok("d3d11-create", detail);

    /* ---------------------------------------------------------------- */
    stage_begin("dxgi-factory");
    SetLastError(0);
    hr = CreateDXGIFactory(&IID_IDXGIFactory, (void**)&factory);
    captured = GetLastError();
    if (FAILED(hr) || factory == NULL) {
        snprintf(detail, sizeof(detail), "hr=0x%08lx", (unsigned long)hr);
        stage_fail("dxgi-factory", captured, detail);
        return BOXEDVN_EXIT_DXGI_FACTORY;
    }
    stage_ok("dxgi-factory", NULL);

    /* ---------------------------------------------------------------- */
    stage_begin("swapchain");
    ZeroMemory(&swap_desc, sizeof(swap_desc));
    swap_desc.BufferCount = 1;
    swap_desc.BufferDesc.Width = kProbeWidth;
    swap_desc.BufferDesc.Height = kProbeHeight;
    swap_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_desc.BufferDesc.RefreshRate.Numerator = 60;
    swap_desc.BufferDesc.RefreshRate.Denominator = 1;
    swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_desc.OutputWindow = window;
    swap_desc.SampleDesc.Count = 1;
    swap_desc.Windowed = TRUE;
    SetLastError(0);
    hr = IDXGIFactory_CreateSwapChain(factory, (IUnknown*)device, &swap_desc,
                                      &swapchain);
    captured = GetLastError();
    if (FAILED(hr) || swapchain == NULL) {
        snprintf(detail, sizeof(detail), "hr=0x%08lx", (unsigned long)hr);
        stage_fail("swapchain", captured, detail);
        return BOXEDVN_EXIT_SWAPCHAIN;
    }
    stage_ok("swapchain", NULL);

    /* ---------------------------------------------------------------- */
    stage_begin("render-target");
    SetLastError(0);
    hr = IDXGISwapChain_GetBuffer(swapchain, 0, &IID_ID3D11Texture2D,
                                  (void**)&backbuffer);
    captured = GetLastError();
    if (FAILED(hr) || backbuffer == NULL) {
        snprintf(detail, sizeof(detail), "hr=0x%08lx stage=getbuffer",
                 (unsigned long)hr);
        stage_fail("render-target", captured, detail);
        return BOXEDVN_EXIT_RENDER_TARGET;
    }
    SetLastError(0);
    hr = ID3D11Device_CreateRenderTargetView(device,
                                             (ID3D11Resource*)backbuffer, NULL,
                                             &rtv);
    captured = GetLastError();
    if (FAILED(hr) || rtv == NULL) {
        snprintf(detail, sizeof(detail), "hr=0x%08lx stage=rtv",
                 (unsigned long)hr);
        stage_fail("render-target", captured, detail);
        return BOXEDVN_EXIT_RENDER_TARGET;
    }
    /* The ported demo draws with a depth buffer. Failing to build one is
     * reported and survived: the cube is convex and back faces are culled, so
     * the picture is the same, and a depth-buffer defect should not end a run
     * that could still say something about the draw. */
    {
        D3D11_TEXTURE2D_DESC depth_desc;
        ID3D11Texture2D_GetDesc(backbuffer, &depth_desc);
        depth_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depth_desc.Usage = D3D11_USAGE_DEFAULT;
        depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        depth_desc.CPUAccessFlags = 0;
        depth_desc.MiscFlags = 0;
        depth_desc.MipLevels = 1;
        depth_desc.ArraySize = 1;
        SetLastError(0);
        hr = ID3D11Device_CreateTexture2D(device, &depth_desc, NULL,
                                          &depth_texture);
        captured = GetLastError();
        if (FAILED(hr) || depth_texture == NULL) {
            depth_texture = NULL;
            snprintf(detail, sizeof(detail), "hr=0x%08lx stage=depth-texture",
                     (unsigned long)hr);
            stage_fail("render-target-depth", captured, detail);
        } else {
            SetLastError(0);
            hr = ID3D11Device_CreateDepthStencilView(
                device, (ID3D11Resource*)depth_texture, NULL, &depth_view);
            captured = GetLastError();
            if (FAILED(hr) || depth_view == NULL) {
                depth_view = NULL;
                snprintf(detail, sizeof(detail), "hr=0x%08lx stage=depth-view",
                         (unsigned long)hr);
                stage_fail("render-target-depth", captured, detail);
            } else {
                stage_ok("render-target-depth", "format=D24S8");
            }
        }
    }
    ID3D11DeviceContext_OMSetRenderTargets(context, 1, &rtv, depth_view);
    snprintf(detail, sizeof(detail), "depth=%s",
             depth_view != NULL ? "yes" : "none");
    stage_ok("render-target", detail);

    /* ---------------------------------------------------------------- */
    /* Shaders and the input layout. The DXBC was compiled ahead of time; the
     * only compiler that runs here is DXMT's DXBC-to-Metal translation, which
     * happens inside these calls or at the first draw. Each object is a
     * separate reported step so the log names the one that fails. */
    stage_begin("shaders");
    SetLastError(0);
    hr = ID3D11Device_CreateVertexShader(device, kProbeVertexShader,
                                         sizeof(kProbeVertexShader), NULL,
                                         &vertex_shader);
    captured = GetLastError();
    if (FAILED(hr) || vertex_shader == NULL) {
        snprintf(detail, sizeof(detail), "hr=0x%08lx stage=vertex-shader",
                 (unsigned long)hr);
        stage_fail("shaders", captured, detail);
        return BOXEDVN_EXIT_SHADERS;
    }
    SetLastError(0);
    hr = ID3D11Device_CreatePixelShader(device, kProbePixelShader,
                                        sizeof(kProbePixelShader), NULL,
                                        &pixel_shader);
    captured = GetLastError();
    if (FAILED(hr) || pixel_shader == NULL) {
        snprintf(detail, sizeof(detail), "hr=0x%08lx stage=pixel-shader",
                 (unsigned long)hr);
        stage_fail("shaders", captured, detail);
        return BOXEDVN_EXIT_SHADERS;
    }
    {
        /* One element, "POS": the ported shader derives the colour from the
         * position, so the vertex buffer carries nothing else. */
        D3D11_INPUT_ELEMENT_DESC layout[1];
        ZeroMemory(layout, sizeof(layout));
        layout[0].SemanticName = "POS";
        layout[0].Format = DXGI_FORMAT_R32G32B32_FLOAT;
        layout[0].AlignedByteOffset = 0;
        layout[0].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
        SetLastError(0);
        hr = ID3D11Device_CreateInputLayout(device, layout, 1,
                                            kProbeVertexShader,
                                            sizeof(kProbeVertexShader),
                                            &input_layout);
        captured = GetLastError();
        if (FAILED(hr) || input_layout == NULL) {
            snprintf(detail, sizeof(detail), "hr=0x%08lx stage=input-layout",
                     (unsigned long)hr);
            stage_fail("shaders", captured, detail);
            return BOXEDVN_EXIT_SHADERS;
        }
    }
    snprintf(detail, sizeof(detail), "vs=%u ps=%u bytes hlsl=%.12s",
             (unsigned)sizeof(kProbeVertexShader),
             (unsigned)sizeof(kProbePixelShader), BOXEDVN_PROBE_HLSL_SHA256);
    stage_ok("shaders", detail);

    /* ---------------------------------------------------------------- */
    stage_begin("geometry");
    {
        D3D11_BUFFER_DESC desc;
        D3D11_SUBRESOURCE_DATA data;

        ZeroMemory(&desc, sizeof(desc));
        ZeroMemory(&data, sizeof(data));
        desc.ByteWidth = sizeof(kCubeVertices);
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        data.pSysMem = kCubeVertices;
        SetLastError(0);
        hr = ID3D11Device_CreateBuffer(device, &desc, &data, &vertex_buffer);
        captured = GetLastError();
        if (FAILED(hr) || vertex_buffer == NULL) {
            snprintf(detail, sizeof(detail), "hr=0x%08lx stage=vertex-buffer",
                     (unsigned long)hr);
            stage_fail("geometry", captured, detail);
            return BOXEDVN_EXIT_GEOMETRY;
        }

        ZeroMemory(&desc, sizeof(desc));
        desc.ByteWidth = sizeof(kCubeIndices);
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
        data.pSysMem = kCubeIndices;
        SetLastError(0);
        hr = ID3D11Device_CreateBuffer(device, &desc, &data, &index_buffer);
        captured = GetLastError();
        if (FAILED(hr) || index_buffer == NULL) {
            snprintf(detail, sizeof(detail), "hr=0x%08lx stage=index-buffer",
                     (unsigned long)hr);
            stage_fail("geometry", captured, detail);
            return BOXEDVN_EXIT_GEOMETRY;
        }

        /* DYNAMIC and written with Map/Unmap every frame, the way the ported
         * demo does it. (The probe used to write this buffer with
         * UpdateSubresource; a device run took an access violation reading
         * that method's vtable slot, so following the demo here also removes
         * that call site.) A constant buffer's size must be a multiple of 16,
         * which a 4x4 float matrix already is. */
        ZeroMemory(&desc, sizeof(desc));
        desc.ByteWidth =
            (UINT)((sizeof(struct probe_matrix) + 15u) & ~(size_t)15u);
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        SetLastError(0);
        hr = ID3D11Device_CreateBuffer(device, &desc, NULL, &constant_buffer);
        captured = GetLastError();
        if (FAILED(hr) || constant_buffer == NULL) {
            snprintf(detail, sizeof(detail),
                     "hr=0x%08lx stage=constant-buffer", (unsigned long)hr);
            stage_fail("geometry", captured, detail);
            return BOXEDVN_EXIT_GEOMETRY;
        }
    }
    /* The demo's own rasterizer and depth-stencil state: counter-clockwise
     * front faces (its cube is wound that way), back faces culled, depth
     * tested and written with LESS. Both are best-effort in the same sense as
     * the depth buffer: a NULL state is Direct3D's default state, which still
     * draws the cube. */
    stage_begin("geometry-state");
    {
        D3D11_RASTERIZER_DESC rasterizer_desc;
        D3D11_DEPTH_STENCIL_DESC depth_stencil_desc;

        ZeroMemory(&rasterizer_desc, sizeof(rasterizer_desc));
        rasterizer_desc.FillMode = D3D11_FILL_SOLID;
        rasterizer_desc.CullMode = D3D11_CULL_BACK;
        rasterizer_desc.FrontCounterClockwise = TRUE;
        rasterizer_desc.DepthClipEnable = TRUE;
        SetLastError(0);
        hr = ID3D11Device_CreateRasterizerState(device, &rasterizer_desc,
                                                &rasterizer_state);
        captured = GetLastError();
        if (FAILED(hr) || rasterizer_state == NULL) {
            rasterizer_state = NULL;
            snprintf(detail, sizeof(detail), "hr=0x%08lx stage=rasterizer-state",
                     (unsigned long)hr);
            stage_fail("geometry-state", captured, detail);
        }

        ZeroMemory(&depth_stencil_desc, sizeof(depth_stencil_desc));
        depth_stencil_desc.DepthEnable = TRUE;
        depth_stencil_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        depth_stencil_desc.DepthFunc = D3D11_COMPARISON_LESS;
        SetLastError(0);
        hr = ID3D11Device_CreateDepthStencilState(device, &depth_stencil_desc,
                                                  &depth_state);
        captured = GetLastError();
        if (FAILED(hr) || depth_state == NULL) {
            depth_state = NULL;
            snprintf(detail, sizeof(detail),
                     "hr=0x%08lx stage=depth-stencil-state", (unsigned long)hr);
            stage_fail("geometry-state", captured, detail);
        }
        snprintf(detail, sizeof(detail), "rasterizer=%s depth_stencil=%s",
                 rasterizer_state != NULL ? "yes" : "default",
                 depth_state != NULL ? "yes" : "default");
        stage_ok("geometry-state", detail);
    }
    snprintf(detail, sizeof(detail), "vertices=%u indices=%u",
             (unsigned)(sizeof(kCubeVertices) / sizeof(kCubeVertices[0])),
             (unsigned)(sizeof(kCubeIndices) / sizeof(kCubeIndices[0])));
    stage_ok("geometry", detail);

    /* Pipeline state that does not change per frame. Bound once, before the
     * loop; Direct3D 11 has no default viewport, so it is set explicitly. */
    {
        const UINT stride = sizeof(struct probe_vertex);
        const UINT offset = 0;
        D3D11_VIEWPORT viewport;
        ZeroMemory(&viewport, sizeof(viewport));
        viewport.Width = (FLOAT)kProbeWidth;
        viewport.Height = (FLOAT)kProbeHeight;
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        ID3D11DeviceContext_RSSetViewports(context, 1, &viewport);
        ID3D11DeviceContext_RSSetState(context, rasterizer_state);
        ID3D11DeviceContext_OMSetDepthStencilState(context, depth_state, 0);
        ID3D11DeviceContext_IASetInputLayout(context, input_layout);
        ID3D11DeviceContext_IASetVertexBuffers(context, 0, 1, &vertex_buffer,
                                               &stride, &offset);
        ID3D11DeviceContext_IASetIndexBuffer(context, index_buffer,
                                             DXGI_FORMAT_R16_UINT, 0);
        ID3D11DeviceContext_IASetPrimitiveTopology(
            context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(context, vertex_shader, NULL, 0);
        ID3D11DeviceContext_VSSetConstantBuffers(context, 0, 1,
                                                 &constant_buffer);
        ID3D11DeviceContext_PSSetShader(context, pixel_shader, NULL, 0);
    }

    /* ---------------------------------------------------------------- */
    /* The presentation loop. It announces its FIRST present, the end of the
     * acceptance run, and nothing else per frame: a per-frame marker would be
     * the largest thing in the log and would say no more than these do.
     *
     * The loop runs until the window is closed. The earlier 240-frame cut-off
     * made the picture freeze on device after four seconds and looked like a
     * hang. Present is asked for no vertical sync (interval 0) so the host's
     * own frame limiter setting, not this program, decides the frame rate --
     * which is exactly why the cube's angle comes from the performance
     * counter and not from the frame number. The demo's clear colour is a
     * constant for the same reason: it used to drift with the frame counter,
     * which strobed once the guest ran unthrottled. */
    stage_begin("present");
    QueryPerformanceFrequency(&counter_frequency);
    QueryPerformanceCounter(&counter_start);
    if (counter_frequency.QuadPart <= 0) {
        counter_frequency.QuadPart = 1;
    }
    for (frames = 0; ; ++frames) {
        MSG message;
        FLOAT colour[4];
        struct probe_matrix mvp;
        D3D11_MAPPED_SUBRESOURCE mapped;
        LARGE_INTEGER counter_now;
        double seconds;
        int quit = 0;
        colour[0] = 0.10f;
        colour[1] = 0.20f;
        colour[2] = 0.60f;
        colour[3] = 1.0f;
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                quit = 1;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (quit) {
            break;
        }
        QueryPerformanceCounter(&counter_now);
        seconds = (double)(counter_now.QuadPart - counter_start.QuadPart) /
                  (double)counter_frequency.QuadPart;
        mvp = cube_transform(seconds);
        ZeroMemory(&mapped, sizeof(mapped));
        hr = ID3D11DeviceContext_Map(context, (ID3D11Resource*)constant_buffer,
                                     0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr) && mapped.pData != NULL) {
            CopyMemory(mapped.pData, &mvp, sizeof(mvp));
            ID3D11DeviceContext_Unmap(context,
                                      (ID3D11Resource*)constant_buffer, 0);
        } else if (!mapped_failed) {
            mapped_failed = 1;
            snprintf(detail, sizeof(detail), "hr=0x%08lx stage=map-constants",
                     (unsigned long)hr);
            stage_fail("present", GetLastError(), detail);
        }
        ID3D11DeviceContext_ClearRenderTargetView(context, rtv, colour);
        if (depth_view != NULL) {
            ID3D11DeviceContext_ClearDepthStencilView(context, depth_view,
                                                      D3D11_CLEAR_DEPTH, 1.0f,
                                                      0);
        }
        ID3D11DeviceContext_DrawIndexed(
            context, (UINT)(sizeof(kCubeIndices) / sizeof(kCubeIndices[0])),
            0, 0);
        SetLastError(0);
        hr = IDXGISwapChain_Present(swapchain, 0, 0);
        captured = GetLastError();
        if (FAILED(hr)) {
            snprintf(detail, sizeof(detail), "hr=0x%08lx frame=%d",
                     (unsigned long)hr, frames);
            stage_fail("present", captured, detail);
            return BOXEDVN_EXIT_PRESENT;
        }
        if (!presented) {
            presented = 1;
            snprintf(detail, sizeof(detail), "hr=0x%08lx first-frame",
                     (unsigned long)hr);
            stage_ok("present", detail);
        }
        if (frames + 1 == kProbeAcceptanceFrames) {
            stage_line("complete ok");
        }
    }

    snprintf(detail, sizeof(detail), "exit ok frames=%d", frames);
    stage_line(detail);
    if (depth_state != NULL) {
        ID3D11DepthStencilState_Release(depth_state);
    }
    if (rasterizer_state != NULL) {
        ID3D11RasterizerState_Release(rasterizer_state);
    }
    if (depth_view != NULL) {
        ID3D11DepthStencilView_Release(depth_view);
    }
    if (depth_texture != NULL) {
        ID3D11Texture2D_Release(depth_texture);
    }
    if (constant_buffer != NULL) {
        ID3D11Buffer_Release(constant_buffer);
    }
    if (index_buffer != NULL) {
        ID3D11Buffer_Release(index_buffer);
    }
    if (vertex_buffer != NULL) {
        ID3D11Buffer_Release(vertex_buffer);
    }
    if (input_layout != NULL) {
        ID3D11InputLayout_Release(input_layout);
    }
    if (pixel_shader != NULL) {
        ID3D11PixelShader_Release(pixel_shader);
    }
    if (vertex_shader != NULL) {
        ID3D11VertexShader_Release(vertex_shader);
    }
    if (rtv != NULL) {
        ID3D11RenderTargetView_Release(rtv);
    }
    if (backbuffer != NULL) {
        ID3D11Texture2D_Release(backbuffer);
    }
    if (swapchain != NULL) {
        IDXGISwapChain_Release(swapchain);
    }
    if (factory != NULL) {
        IDXGIFactory_Release(factory);
    }
    if (context != NULL) {
        ID3D11DeviceContext_Release(context);
    }
    if (device != NULL) {
        ID3D11Device_Release(device);
    }
    return BOXEDVN_EXIT_OK;
}
