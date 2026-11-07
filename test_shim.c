/*
 * Native smoke test for the ddraw.dll shim.  Runs the exact call sequence
 * the Enhanced build's dshow.dll uses, plus the Blt/BltFast/Flip/GetDC
 * fallback paths, against the shim on plain Windows.  If any vtable stub
 * had the wrong __stdcall arity this process would crash before finishing.
 *
 * The window layout mimics the game: an unowned main window plus a visible
 * popup owned by it (the shim must pick the popup).
 *
 * Usage: test_shim.exe [path\to\ddraw.dll]
 */
#include <windows.h>
#include <ddraw.h>
#include <stdio.h>

typedef HRESULT (WINAPI *DDCREATE)(GUID *, LPDIRECTDRAW *, IUnknown *);

static int failures = 0;
#define CHECK(cond) do { \
    if (cond) { printf("ok: %s\n", #cond); } \
    else { printf("FAIL: %s\n", #cond); failures++; } \
} while (0)

static LRESULT CALLBACK wp(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return DefWindowProcA(h, m, w, l);
}

int main(int argc, char **argv)
{
    const char *dllpath = argc > 1 ? argv[1] : "ddraw.dll";
    WNDCLASSA wc;
    HWND mainw, popup;
    HMODULE dll;
    DDCREATE pCreate;
    LPDIRECTDRAW dd = NULL;
    LPDIRECTDRAWSURFACE primary = NULL, off = NULL;
    DDSURFACEDESC d;
    HRESULT hr;
    MSG msg;
    int i;

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc   = wp;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.lpszClassName = "WCPShimTest";
    RegisterClassA(&wc);
    mainw = CreateWindowExA(0, "WCPShimTest", "main",
                            WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                            100, 100, 320, 200, NULL, NULL, wc.hInstance, NULL);
    popup = CreateWindowExA(0, "WCPShimTest", "video",
                            WS_POPUP | WS_VISIBLE,
                            200, 200, 640, 480, mainw, NULL, wc.hInstance, NULL);
    ShowWindow(mainw, SW_SHOW);
    ShowWindow(popup, SW_SHOW);

    dll = LoadLibraryA(dllpath);
    CHECK(dll != NULL);
    if (!dll) { printf("cannot load %s (err %lu)\n", dllpath, GetLastError()); return 1; }

    pCreate = (DDCREATE)GetProcAddress(dll, "DirectDrawCreate");
    CHECK(pCreate != NULL);
    CHECK(GetProcAddress(dll, "DirectDrawCreateEx") != NULL);
    CHECK(GetProcAddress(dll, "DirectDrawCreateClipper") != NULL);
    CHECK(GetProcAddress(dll, "DirectDrawEnumerateA") != NULL);
    CHECK(GetProcAddress(dll, "DirectDrawEnumerateExW") != NULL);
    CHECK(GetProcAddress(dll, "DllGetClassObject") != NULL);
    CHECK(GetProcAddress(dll, "GetSurfaceFromDC") != NULL);
    if (!pCreate) return 1;

    hr = pCreate(NULL, &dd, NULL);
    CHECK(hr == DD_OK && dd != NULL);
    if (!dd) return 1;

    hr = dd->lpVtbl->SetCooperativeLevel(dd, NULL, DDSCL_NORMAL);
    CHECK(hr == DD_OK);

    /* primary surface, exactly how the renderer asks for it */
    memset(&d, 0, sizeof d);
    d.dwSize = sizeof d;
    d.dwFlags = DDSD_CAPS;
    d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    hr = dd->lpVtbl->CreateSurface(dd, &d, &primary, NULL);
    CHECK(hr == DD_OK && primary != NULL);
    if (!primary) return 1;

    /* lock / fill / unlock a few frames */
    for (i = 0; i < 3; i++) {
        memset(&d, 0, sizeof d);
        d.dwSize = sizeof d;
        hr = primary->lpVtbl->Lock(primary, NULL, &d, 0, NULL);
        CHECK(hr == DD_OK);
        CHECK(d.lpSurface != NULL);
        CHECK(d.lPitch == (LONG)GetSystemMetrics(SM_CXSCREEN) * 4);
        CHECK((d.ddpfPixelFormat.dwFlags & DDPF_RGB) && d.ddpfPixelFormat.dwRGBBitCount == 32);
        if (hr == DD_OK && d.lpSurface) {
            DWORD *px = (DWORD *)d.lpSurface;
            LONG x, y;
            for (y = 0; y < 200; y++)
                for (x = 0; x < 200; x++)
                    px[y * (d.lPitch / 4) + x] = (DWORD)((x << 8) | y | (i << 20));
        }
        hr = primary->lpVtbl->Unlock(primary, d.lpSurface);
        CHECK(hr == DD_OK);
    }

    /* offscreen -> primary Blt / BltFast path */
    memset(&d, 0, sizeof d);
    d.dwSize = sizeof d;
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN;
    d.dwWidth = 320; d.dwHeight = 240;
    hr = dd->lpVtbl->CreateSurface(dd, &d, &off, NULL);
    CHECK(hr == DD_OK && off != NULL);
    if (off) {
        memset(&d, 0, sizeof d);
        d.dwSize = sizeof d;
        hr = off->lpVtbl->Lock(off, NULL, &d, 0, NULL);
        CHECK(hr == DD_OK && d.lpSurface != NULL && d.lPitch == 320 * 4);
        if (d.lpSurface) memset(d.lpSurface, 0x80, (size_t)d.lPitch * 240);
        off->lpVtbl->Unlock(off, d.lpSurface);
        hr = primary->lpVtbl->Blt(primary, NULL, off, NULL, 0, NULL);
        CHECK(hr == DD_OK);
        hr = primary->lpVtbl->BltFast(primary, 10, 10, off, NULL, 0);
        CHECK(hr == DD_OK);
        /* color-fill Blt */
        {
            DDBLTFX fx;
            memset(&fx, 0, sizeof fx);
            fx.dwSize = sizeof fx;
            fx.dwFillColor = 0x000000FF;
            hr = primary->lpVtbl->Blt(primary, NULL, NULL, NULL, DDBLT_COLORFILL, &fx);
            CHECK(hr == DD_OK);
        }
    }

    /* flip chain on a second DirectDraw object */
    {
        LPDIRECTDRAW dd2 = NULL;
        LPDIRECTDRAWSURFACE dd2primary = NULL, back = NULL;
        hr = pCreate(NULL, &dd2, NULL);
        CHECK(hr == DD_OK && dd2 != NULL);
        memset(&d, 0, sizeof d);
        d.dwSize = sizeof d;
        d.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
        d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_COMPLEX | DDSCAPS_FLIP;
        d.dwBackBufferCount = 1;
        hr = dd2->lpVtbl->CreateSurface(dd2, &d, &dd2primary, NULL);
        CHECK(hr == DD_OK && dd2primary != NULL);
        if (dd2primary) {
            DDSCAPS caps;
            caps.dwCaps = DDSCAPS_BACKBUFFER;
            hr = dd2primary->lpVtbl->GetAttachedSurface(dd2primary, &caps, &back);
            CHECK(hr == DD_OK && back != NULL);
            if (back) {
                memset(&d, 0, sizeof d);
                d.dwSize = sizeof d;
                hr = back->lpVtbl->Lock(back, NULL, &d, 0, NULL);
                CHECK(hr == DD_OK && d.lpSurface != NULL);
                if (d.lpSurface) memset(d.lpSurface, 0xFF, (size_t)d.lPitch * 100);
                back->lpVtbl->Unlock(back, d.lpSurface);
                hr = dd2primary->lpVtbl->Flip(dd2primary, NULL, 0);
                CHECK(hr == DD_OK);
            }
            /* unsupported interface must fail cleanly, not crash */
            {
                void *pv = (void *)0xdeadbeef;
                hr = dd2primary->lpVtbl->QueryInterface(dd2primary, &IID_IDirectDrawSurface4, &pv);
                CHECK(hr == E_NOINTERFACE && pv == NULL);
            }
            /* GetDC / ReleaseDC path */
            {
                HDC hdc = NULL;
                hr = dd2primary->lpVtbl->GetDC(dd2primary, &hdc);
                CHECK(hr == DD_OK && hdc != NULL);
                if (hdc) {
                    SetPixel(hdc, 5, 5, RGB(255, 0, 0));
                    hr = dd2primary->lpVtbl->ReleaseDC(dd2primary, hdc);
                    CHECK(hr == DD_OK);
                }
            }
            dd2primary->lpVtbl->Release(dd2primary);
        }
        dd2->lpVtbl->Release(dd2);
    }

    if (off) off->lpVtbl->Release(off);
    primary->lpVtbl->Release(primary);
    dd->lpVtbl->Release(dd);

    /* let the windows paint a little */
    for (i = 0; i < 50; i++) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        Sleep(10);
    }

    FreeLibrary(dll);
    if (failures) printf("TEST FAILURES: %d\n", failures);
    else          printf("ALL TESTS PASSED\n");
    return failures ? 1 : 0;
}
