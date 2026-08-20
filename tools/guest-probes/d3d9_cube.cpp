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

    const float seconds = static_cast<float>(GetTickCount() - gStartedAt) / 1000.0f;
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
    gVertexBuffer->Unlock();
    return true;
}

bool renderFrame() {
    if (!updateVertices()) {
        return false;
    }

    gDevice->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                   D3DCOLOR_XRGB(23, 28, 42), 1.0f, 0);
    if (SUCCEEDED(gDevice->BeginScene())) {
        gDevice->SetStreamSource(0, gVertexBuffer, 0, sizeof(Vertex));
        gDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 12);
        gDevice->EndScene();
    }
    return SUCCEEDED(gDevice->Present(nullptr, nullptr, nullptr, nullptr));
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
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                running = false;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (running && !renderFrame()) {
            Sleep(16);
        }
    }

    releaseGraphics();
    UnregisterClassW(kWindowClass, instance);
    return static_cast<int>(message.wParam);
}
