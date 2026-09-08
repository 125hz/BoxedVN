/*
 * BoxedVN 32-bit Direct3D 9 graphics probe.
 *
 * This intentionally uses only Win32 and D3D9 APIs supplied by Wine.  Keeping
 * the guest tiny and dependency-free makes it an acceptance test for the
 * complete BoxedWine -> WineD3D -> Vulkan -> MoltenVK path rather than for a
 * particular application runtime.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <math.h>
#include <cstdio>
#include <cstring>
#include "x87_probe.h"
#include "d3d9_color_probe.h"

namespace {

constexpr wchar_t kWindowClass[] = L"BoxedVNGraphicsProbe";
constexpr DWORD kVertexFormat = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;

struct Vertex {
    float x;
    float y;
    float z;
    float rhw;
    D3DCOLOR color;
};

struct Vec3 {
    float x;
    float y;
    float z;
};

constexpr Vec3 kCorners[] = {
    {-1.0f, -1.0f, -1.0f}, { 1.0f, -1.0f, -1.0f},
    { 1.0f,  1.0f, -1.0f}, {-1.0f,  1.0f, -1.0f},
    {-1.0f, -1.0f,  1.0f}, { 1.0f, -1.0f,  1.0f},
    { 1.0f,  1.0f,  1.0f}, {-1.0f,  1.0f,  1.0f},
};

constexpr unsigned char kTriangles[] = {
    0, 1, 2, 0, 2, 3,  // back
    5, 4, 7, 5, 7, 6,  // front
    4, 0, 3, 4, 3, 7,  // left
    1, 5, 6, 1, 6, 2,  // right
    3, 2, 6, 3, 6, 7,  // top
    4, 5, 1, 4, 1, 0,  // bottom
};

constexpr D3DCOLOR kFaceColors[] = {
    D3DCOLOR_XRGB(70, 120, 255),
    D3DCOLOR_XRGB(255, 80, 150),
    D3DCOLOR_XRGB(80, 230, 150),
    D3DCOLOR_XRGB(255, 190, 60),
    D3DCOLOR_XRGB(180, 90, 255),
    D3DCOLOR_XRGB(80, 220, 240),
};

IDirect3D9* gDirect3D = nullptr;
IDirect3DDevice9* gDevice = nullptr;
IDirect3DVertexBuffer9* gVertexBuffer = nullptr;
DWORD gStartedAt = 0;
unsigned int gWidth = 960;
unsigned int gHeight = 600;

void releaseGraphics() {
    if (gVertexBuffer) {
        gVertexBuffer->Release();
        gVertexBuffer = nullptr;
    }
    if (gDevice) {
        gDevice->Release();
        gDevice = nullptr;
    }
    if (gDirect3D) {
        gDirect3D->Release();
        gDirect3D = nullptr;
    }
}

void reportFailure(HWND window, const wchar_t* operation, HRESULT result) {
    wchar_t detail[256] = {};
    wsprintfW(detail, L"%ls failed (HRESULT 0x%08lX).", operation,
              static_cast<unsigned long>(result));
    MessageBoxW(window, detail, L"Direct3D 9 graphics probe", MB_OK | MB_ICONERROR);
}

bool createGraphics(HWND window) {
    gDirect3D = Direct3DCreate9(D3D_SDK_VERSION);
    if (!gDirect3D) {
        reportFailure(window, L"Direct3DCreate9", E_FAIL);
        return false;
    }

    D3DPRESENT_PARAMETERS present = {};
    present.Windowed = TRUE;
    present.SwapEffect = D3DSWAPEFFECT_DISCARD;
    present.BackBufferFormat = D3DFMT_UNKNOWN;
    present.EnableAutoDepthStencil = TRUE;
    present.AutoDepthStencilFormat = D3DFMT_D16;
    present.PresentationInterval = D3DPRESENT_INTERVAL_ONE;

    HRESULT result = gDirect3D->CreateDevice(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &present, &gDevice);
    if (FAILED(result)) {
        reportFailure(window, L"IDirect3D9::CreateDevice", result);
        return false;
    }

    result = gDevice->CreateVertexBuffer(
        sizeof(Vertex) * 36, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
        kVertexFormat, D3DPOOL_DEFAULT, &gVertexBuffer, nullptr);
    if (FAILED(result)) {
        reportFailure(window, L"IDirect3DDevice9::CreateVertexBuffer", result);
        return false;
    }

    gDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    gDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    gDevice->SetRenderState(D3DRS_ZENABLE, TRUE);
    gDevice->SetFVF(kVertexFormat);
    probeD3D9Colors(gDevice);
    gStartedAt = GetTickCount();
    return true;
}

Vec3 rotate(Vec3 input, float yaw, float pitch) {
    const float sinYaw = sinf(yaw);
    const float cosYaw = cosf(yaw);
    const float sinPitch = sinf(pitch);
    const float cosPitch = cosf(pitch);

    const float x = input.x * cosYaw + input.z * sinYaw;
    const float z = -input.x * sinYaw + input.z * cosYaw;
    return {x, input.y * cosPitch - z * sinPitch,
            input.y * sinPitch + z * cosPitch};
}

bool updateVertices() {
    Vertex* vertices = nullptr;
    HRESULT result = gVertexBuffer->Lock(
        0, 0, reinterpret_cast<void**>(&vertices), D3DLOCK_DISCARD);
    if (FAILED(result)) {
        return false;
    }

    const DWORD tick = GetTickCount();
    const float seconds = static_cast<float>(tick - gStartedAt) / 1000.0f;
    const float scale = static_cast<float>(gHeight) * 0.72f;
    for (unsigned int triangleVertex = 0; triangleVertex < 36; ++triangleVertex) {
        const Vec3 rotated = rotate(kCorners[kTriangles[triangleVertex]],
                                    seconds * 0.75f, seconds * 0.43f);
        const float cameraZ = rotated.z + 4.2f;
        Vertex& output = vertices[triangleVertex];
        output.x = static_cast<float>(gWidth) * 0.5f + rotated.x * scale / cameraZ;
        output.y = static_cast<float>(gHeight) * 0.5f - rotated.y * scale / cameraZ;
        output.z = (cameraZ - 2.0f) / 8.0f;
        output.rhw = 1.0f / cameraZ;
        output.color = kFaceColors[triangleVertex / 6];
    }
    // A successful Present is not proof that the clock, x87 math and dynamic
    // vertex upload all advanced. Keep independent clock/vertex witnesses in
    // the probe, bounded even when one of those clocks stops progressing.
    static unsigned frames = 0;
    static unsigned reports = 0;
    static unsigned long long lastWall = 0;
    ++frames;
    if (reports < 16 && (frames <= 2 || (frames & 63) == 0)) {
        FILETIME fileTime;
        GetSystemTimeAsFileTime(&fileTime);
        const unsigned long long wall =
            (static_cast<unsigned long long>(fileTime.dwHighDateTime) << 32) |
            fileTime.dwLowDateTime;
        if (frames <= 2 || wall - lastWall >= 50000000ULL) {
            lastWall = wall;
            ++reports;
            LARGE_INTEGER qpc = {}, frequency = {};
            QueryPerformanceCounter(&qpc);
            QueryPerformanceFrequency(&frequency);
            unsigned bits[4];
            std::memcpy(bits, vertices, sizeof(bits));
            char report[320];
            const int length = std::snprintf(report, sizeof(report),
                "BOXEDWINE_PE32_ANIMATION frame=%u tick=%lu elapsed=%lu "
                "qpc=%lld frequency=%lld wall=%llu vertex=%08x,%08x,%08x,%08x\n",
                frames, static_cast<unsigned long>(tick),
                static_cast<unsigned long>(tick - gStartedAt),
                qpc.QuadPart, frequency.QuadPart, wall,
                bits[0], bits[1], bits[2], bits[3]);
            if (length > 0 && length < int(sizeof(report))) {
                DWORD written;
                WriteFile(GetStdHandle(STD_ERROR_HANDLE), report, DWORD(length), &written, nullptr);
            }
        }
    }
    return SUCCEEDED(gVertexBuffer->Unlock());
}

struct FrameTiming {
    unsigned frame = 0;
    LONGLONG begin = 0, messages = 0;
    bool active() const { return frame <= 16; }
    LONGLONG stamp() const {
        LARGE_INTEGER value = {};
        if (active()) QueryPerformanceCounter(&value);
        return value.QuadPart;
    }
} gFrameTiming;

bool renderFrame() {
    if (!updateVertices()) {
        return false;
    }

    const auto uploaded = gFrameTiming.stamp();
    gDevice->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                   D3DCOLOR_XRGB(23, 28, 42), 1.0f, 0);
    if (SUCCEEDED(gDevice->BeginScene())) {
        gDevice->SetStreamSource(0, gVertexBuffer, 0, sizeof(Vertex));
        gDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 12);
        gDevice->EndScene();
    }
    const auto drawn = gFrameTiming.stamp();
    const HRESULT result = gDevice->Present(nullptr, nullptr, nullptr, nullptr);
    const auto presented = gFrameTiming.stamp();
    if (gFrameTiming.active()) {
        LARGE_INTEGER frequency = {};
        QueryPerformanceFrequency(&frequency);
        auto micros = [&](LONGLONG from, LONGLONG to) {
            return frequency.QuadPart > 0 ? (to - from) * 1000000 / frequency.QuadPart : -1;
        };
        char report[256];
        const int length = std::snprintf(report, sizeof(report),
            "BOXEDWINE_PE32_FRAME_TIMING frame=%u message_us=%lld upload_us=%lld draw_us=%lld present_us=%lld result=0x%08lx\n",
            gFrameTiming.frame, micros(gFrameTiming.begin, gFrameTiming.messages),
            micros(gFrameTiming.messages, uploaded), micros(uploaded, drawn),
            micros(drawn, presented), static_cast<unsigned long>(result));
        if (length > 0 && length < int(sizeof(report))) {
            DWORD written;
            WriteFile(GetStdHandle(STD_ERROR_HANDLE), report, DWORD(length), &written, nullptr);
        }
    }
    return SUCCEEDED(result);
}

LRESULT CALLBACK windowProcedure(HWND window, UINT message,
                                 WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            gWidth = LOWORD(lParam) ? LOWORD(lParam) : 1;
            gHeight = HIWORD(lParam) ? HIWORD(lParam) : 1;
        }
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int showCommand) {
    const auto math = runX87Probe();
    char mathReport[240];
    const int mathLength = std::snprintf(mathReport, sizeof(mathReport),
        "BOXEDWINE_PE32_X87_PROBE integer=%016llx minimum=%016llx subnormal=%016llx restored=%u pass=%u\n",
        (unsigned long long)math.integerLoad, (unsigned long long)math.minimum,
        (unsigned long long)math.scaledSubnormal, unsigned(math.restored), unsigned(math.passed()));
    if (mathLength > 0 && mathLength < int(sizeof(mathReport))) {
        DWORD written;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), mathReport, DWORD(mathLength), &written, nullptr);
    }
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&windowClass)) {
        return 1;
    }

    RECT bounds = {0, 0, static_cast<LONG>(gWidth), static_cast<LONG>(gHeight)};
    AdjustWindowRect(&bounds, WS_OVERLAPPEDWINDOW, FALSE);
    HWND window = CreateWindowExW(
        0, kWindowClass, L"BoxedVN - 32-bit Direct3D 9 cube",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        bounds.right - bounds.left, bounds.bottom - bounds.top,
        nullptr, nullptr, instance, nullptr);
    if (!window) {
        return 2;
    }

    ShowWindow(window, showCommand);
    UpdateWindow(window);
    if (!createGraphics(window)) {
        releaseGraphics();
        DestroyWindow(window);
        return 3;
    }

    MSG message = {};
    bool running = true;
    while (running) {
        if (gFrameTiming.frame <= 16) ++gFrameTiming.frame;
        gFrameTiming.begin = gFrameTiming.stamp();
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                running = false;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        gFrameTiming.messages = gFrameTiming.stamp();
        if (running && !renderFrame()) {
            Sleep(16);
        }
    }

    releaseGraphics();
    UnregisterClassW(kWindowClass, instance);
    return static_cast<int>(message.wParam);
}
