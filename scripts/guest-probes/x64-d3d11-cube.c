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
 * A spinning cube with a different colour per face, over a slowly changing
 * clear colour. The first device runs of this probe only cleared, which
 * proved presentation but exercised no shader: a clear is a Metal render
 * pass load action, not a pipeline. The cube adds the rest of the path a
 * game needs -- DXBC vertex and pixel shaders translated by DXMT, an input
 * layout, vertex/index/constant buffers, a viewport, and an indexed draw.
 * The shaders are compiled ahead of time (x64-d3d11-cube.hlsl, embedded via
 * x64-d3d11-cube-shaders.h) so no HLSL compiler runs inside the guest.
 *
 * No depth buffer on purpose: the cube is convex and back faces are culled
 * by the default rasterizer state, so it renders correctly without one, and
 * that keeps a depth-buffer defect from masquerading as a draw defect.
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
/* Geometry: a unit cube, one colour per face, indexed as 12 triangles.
 *
 * Direct3D's default rasterizer state treats CLOCKWISE triangles as front
 * faces in its left-handed convention and culls the rest. Every triangle
 * below is wound clockwise as seen from outside the cube, so the convex cube
 * renders correctly with no depth buffer. */
struct probe_vertex {
    float position[3];
    float colour[3];
};

static const struct probe_vertex kCubeVertices[8] = {
    {{-1.0f, -1.0f, -1.0f}, {0.95f, 0.30f, 0.25f}},
    {{-1.0f,  1.0f, -1.0f}, {0.95f, 0.75f, 0.20f}},
    {{ 1.0f,  1.0f, -1.0f}, {0.30f, 0.85f, 0.35f}},
    {{ 1.0f, -1.0f, -1.0f}, {0.25f, 0.55f, 0.95f}},
    {{-1.0f, -1.0f,  1.0f}, {0.80f, 0.35f, 0.90f}},
    {{-1.0f,  1.0f,  1.0f}, {0.95f, 0.95f, 0.95f}},
    {{ 1.0f,  1.0f,  1.0f}, {0.20f, 0.90f, 0.90f}},
    {{ 1.0f, -1.0f,  1.0f}, {0.95f, 0.55f, 0.65f}},
};

static const WORD kCubeIndices[36] = {
    0, 1, 2,  0, 2, 3,  /* -z */
    4, 6, 5,  4, 7, 6,  /* +z */
    4, 5, 1,  4, 1, 0,  /* -x */
    3, 2, 6,  3, 6, 7,  /* +x */
    1, 5, 6,  1, 6, 2,  /* +y */
    4, 0, 3,  4, 3, 7,  /* -y */
};

/* Row-major 4x4 matrices, m[row][column], applied as M * v. The HLSL side
 * declares its matrix row_major so this exact layout is uploaded unchanged. */
struct probe_matrix {
    float m[4][4];
};

static struct probe_matrix matrix_identity(void) {
    struct probe_matrix r;
    int i, j;
    for (i = 0; i < 4; ++i) {
        for (j = 0; j < 4; ++j) {
            r.m[i][j] = (i == j) ? 1.0f : 0.0f;
        }
    }
    return r;
}

static struct probe_matrix matrix_multiply(const struct probe_matrix* a,
                                           const struct probe_matrix* b) {
    struct probe_matrix r;
    int i, j, k;
    for (i = 0; i < 4; ++i) {
        for (j = 0; j < 4; ++j) {
            float sum = 0.0f;
            for (k = 0; k < 4; ++k) {
                sum += a->m[i][k] * b->m[k][j];
            }
            r.m[i][j] = sum;
        }
    }
    return r;
}

static struct probe_matrix matrix_rotation_x(float angle) {
    struct probe_matrix r = matrix_identity();
    float c = cosf(angle), s = sinf(angle);
    r.m[1][1] = c;  r.m[1][2] = -s;
    r.m[2][1] = s;  r.m[2][2] = c;
    return r;
}

static struct probe_matrix matrix_rotation_y(float angle) {
    struct probe_matrix r = matrix_identity();
    float c = cosf(angle), s = sinf(angle);
    r.m[0][0] = c;  r.m[0][2] = s;
    r.m[2][0] = -s; r.m[2][2] = c;
    return r;
}

static struct probe_matrix matrix_translation(float x, float y, float z) {
    struct probe_matrix r = matrix_identity();
    r.m[0][3] = x;
    r.m[1][3] = y;
    r.m[2][3] = z;
    return r;
}

/* Left-handed perspective projection, depth mapped to [0, 1]. */
static struct probe_matrix matrix_perspective(float fov_y, float aspect,
                                              float near_z, float far_z) {
    struct probe_matrix r;
    float y_scale = 1.0f / tanf(fov_y * 0.5f);
    float x_scale = y_scale / aspect;
    int i, j;
    for (i = 0; i < 4; ++i) {
        for (j = 0; j < 4; ++j) {
            r.m[i][j] = 0.0f;
        }
    }
    r.m[0][0] = x_scale;
    r.m[1][1] = y_scale;
    r.m[2][2] = far_z / (far_z - near_z);
    r.m[2][3] = -near_z * far_z / (far_z - near_z);
    r.m[3][2] = 1.0f;
    return r;
}

static struct probe_matrix cube_transform(int frame) {
    const float angle = (float)frame * 0.035f;
    struct probe_matrix rotate_y = matrix_rotation_y(angle);
    struct probe_matrix rotate_x = matrix_rotation_x(angle * 0.6f);
    struct probe_matrix model = matrix_multiply(&rotate_y, &rotate_x);
    struct probe_matrix view = matrix_translation(0.0f, 0.0f, 4.5f);
    struct probe_matrix projection = matrix_perspective(
        1.0471976f /* 60 degrees */, (float)kProbeWidth / (float)kProbeHeight,
        0.5f, 20.0f);
    struct probe_matrix model_view = matrix_multiply(&view, &model);
    return matrix_multiply(&projection, &model_view);
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
    ID3D11VertexShader* vertex_shader = NULL;
    ID3D11PixelShader* pixel_shader = NULL;
    ID3D11InputLayout* input_layout = NULL;
    ID3D11Buffer* vertex_buffer = NULL;
    ID3D11Buffer* index_buffer = NULL;
    ID3D11Buffer* constant_buffer = NULL;
    D3D_FEATURE_LEVEL level = (D3D_FEATURE_LEVEL)0;
    HRESULT hr;
    char detail[320];
    int frames;
    int presented = 0;

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
    ID3D11DeviceContext_OMSetRenderTargets(context, 1, &rtv, NULL);
    stage_ok("render-target", NULL);

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
        D3D11_INPUT_ELEMENT_DESC layout[2];
        ZeroMemory(layout, sizeof(layout));
        layout[0].SemanticName = "POSITION";
        layout[0].Format = DXGI_FORMAT_R32G32B32_FLOAT;
        layout[0].AlignedByteOffset = 0;
        layout[0].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
        layout[1].SemanticName = "COLOR";
        layout[1].Format = DXGI_FORMAT_R32G32B32_FLOAT;
        layout[1].AlignedByteOffset = 12;
        layout[1].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
        SetLastError(0);
        hr = ID3D11Device_CreateInputLayout(device, layout, 2,
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

        ZeroMemory(&desc, sizeof(desc));
        desc.ByteWidth = sizeof(struct probe_matrix);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
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
     * own frame limiter setting, not this program, decides the frame rate. */
    stage_begin("present");
    for (frames = 0; ; ++frames) {
        MSG message;
        FLOAT colour[4];
        struct probe_matrix mvp;
        int quit = 0;
        colour[0] = 0.10f;
        colour[1] = 0.20f + (FLOAT)(frames % 60) / 240.0f;
        colour[2] = 0.45f;
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
        mvp = cube_transform(frames);
        ID3D11DeviceContext_UpdateSubresource(context,
                                              (ID3D11Resource*)constant_buffer,
                                              0, NULL, &mvp, 0, 0);
        ID3D11DeviceContext_ClearRenderTargetView(context, rtv, colour);
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
