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
#include <stdio.h>

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
    BOXEDVN_EXIT_PRESENT = 16
};

static const char kStagePrefix[] = "BOXEDVN_X64_CUBE_STAGE ";

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
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
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
    stage_begin("create-window");
    SetLastError(0);
    window = CreateWindowExW(0, kClassName, L"BoxedVN D3D11 probe",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             640, 480, NULL, NULL, instance, NULL);
    captured = GetLastError();
    if (window == NULL) {
        stage_fail("create-window", captured, NULL);
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
    swap_desc.BufferDesc.Width = 640;
    swap_desc.BufferDesc.Height = 480;
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
    /* The presentation loop. It announces its FIRST present and nothing
     * afterwards: a per-frame marker would be the largest thing in the log and
     * would say no more than this one line does. */
    stage_begin("present");
    for (frames = 0; frames < 240; ++frames) {
        MSG message;
        FLOAT colour[4];
        colour[0] = 0.10f;
        colour[1] = 0.20f + (FLOAT)(frames % 60) / 240.0f;
        colour[2] = 0.45f;
        colour[3] = 1.0f;
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                frames = 240;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        ID3D11DeviceContext_ClearRenderTargetView(context, rtv, colour);
        SetLastError(0);
        hr = IDXGISwapChain_Present(swapchain, 1, 0);
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
    }

    stage_line("complete ok");
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
