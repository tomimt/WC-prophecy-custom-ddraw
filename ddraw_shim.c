/*
 * ddraw.dll replacement shim for Wing Commander Prophecy: 30th Anniversary
 * Edition (Enhanced build) running under Linux/Proton.
 *
 * The Enhanced build's dshow.dll plays the upscaled/master MP4 cutscenes
 * through DirectDraw the 1997 way:
 *
 *   DirectDrawCreate(NULL)
 *   SetCooperativeLevel(hwnd = NULL, DDSCL_NORMAL)        -- no window
 *   CreateSurface(DDSCAPS_PRIMARYSURFACE, desktop res, RGB32)
 *   per frame: Lock -> memcpy decoded frame -> Unlock
 *
 * A primary surface with no cooperative-level window means "write into the
 * visible framebuffer".  Under Wine there is none, so the picture never
 * reaches the screen (audio is a separate filter and keeps playing).
 *
 * This shim serves the primary surface as an ordinary top-down BGRX memory
 * buffer and presents it with SetDIBitsToDevice into the video popup window
 * the engine already created.  That window is found with EnumWindows:
 * same process, visible, and GetWindow(h, GW_OWNER) != NULL -- the engine's
 * main window has no owner, wcphr.dll's video popup does.  Our own topmost
 * window is kept only as a fallback (it stays invisible under KWin/gamescope
 * because the game's active fullscreen window is kept above it).
 *
 * The DLL is deliberately written against Win32 only (no C runtime calls)
 * so it depends on nothing but Wine's builtin kernel32/user32/gdi32.
 *
 * Build (32-bit!):
 *   i686-w64-mingw32-gcc -shared -O2 -fno-builtin -fno-strict-aliasing \
 *       -Wall -Wextra -o ddraw.dll ddraw_shim.c ddraw.def \
 *       -luser32 -lgdi32 -luuid -static-libgcc
 *
 * Install:
 *   copy ddraw.dll into the game's "Enhanced" folder (next to prophecy.exe)
 *   Steam launch options:  WINEDLLOVERRIDES="ddraw=n" %command%
 *   (without the override Wine silently keeps using its builtin ddraw and
 *    this shim is never called -- no error, no log)
 *
 * Log: ddraw_shim.log is written next to the game executable.
 */

#include <windows.h>
#include <ddraw.h>
#include <stdarg.h>

/* Interface GUIDs (normally come from dxguid/libuuid; defined here so the
 * shim has zero external GUID dependencies). */
const GUID IID_IUnknown =
    { 0x00000000, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
const GUID IID_IDirectDraw =
    { 0x6C14DB80, 0xA733, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } };
const GUID IID_IDirectDrawSurface =
    { 0x6C14DB81, 0xA733, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } };

/* ------------------------------------------------------------------------ */
/* freestanding CRT bits (no C runtime linked on purpose)                    */

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s) return dst;
    if (d < s) { while (n--) *d++ = *s++; }
    else { d += n; s += n; while (n--) *--d = *--s; }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    while (n--) { if (*x != *y) return (int)*x - (int)*y; x++; y++; }
    return 0;
}

static void *xalloc(size_t n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n); }
static void  xfree(void *p)   { if (p) HeapFree(GetProcessHeap(), 0, p); }

/* ------------------------------------------------------------------------ */
/* logging                                                                   */

static CRITICAL_SECTION g_cs;
static HANDLE g_hlog;
static int    g_log_tried;

static void log_open(void)
{
    char path[MAX_PATH];
    DWORD n;
    g_log_tried = 1;
    n = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (n && n < MAX_PATH) {
        char *s = path + n;
        while (s > path && s[-1] != '\\') s--;
        lstrcpyA(s, "ddraw_shim.log");
        g_hlog = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (g_hlog == INVALID_HANDLE_VALUE) g_hlog = NULL;
    }
    if (!g_hlog) {
        g_hlog = CreateFileA("ddraw_shim.log", GENERIC_WRITE, FILE_SHARE_READ, NULL,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (g_hlog == INVALID_HANDLE_VALUE) g_hlog = NULL;
    }
}

static void shim_log(const char *fmt, ...)
{
    char body[900];
    char line[1100];
    DWORD written;
    va_list ap;

    EnterCriticalSection(&g_cs);
    if (!g_hlog && !g_log_tried) log_open();
    if (g_hlog) {
        va_start(ap, fmt);
        wvsprintfA(body, fmt, ap);
        va_end(ap);
        wsprintfA(line, "[%lu] %s\r\n", (unsigned long)GetTickCount(), body);
        WriteFile(g_hlog, line, lstrlenA(line), &written, NULL);
    }
    LeaveCriticalSection(&g_cs);
}

/* rate limiting for per-frame entry points: first 5, then every 300th */
static int g_n_lock, g_n_unlock, g_n_blt, g_n_bltfast, g_n_flip, g_n_present;

static int log_maybe(int *counter)
{
    int n = ++*counter;
    return n <= 5 || (n % 300) == 0;
}

/* ------------------------------------------------------------------------ */
/* GUID helpers                                                              */

static void hexn(char *out, DWORD v, int digits)
{
    static const char h[] = "0123456789ABCDEF";
    int i;
    for (i = digits - 1; i >= 0; i--) { out[i] = h[v & 15]; v >>= 4; }
}

static const char *guid_str(const GUID *g, char buf[40])
{
    int i;
    if (!g) { lstrcpyA(buf, "(null)"); return buf; }
    hexn(buf, g->Data1, 8);
    buf[8] = '-';
    hexn(buf + 9, g->Data2, 4);
    buf[13] = '-';
    hexn(buf + 14, g->Data3, 4);
    buf[18] = '-';
    hexn(buf + 19, (DWORD)((g->Data4[0] << 8) | g->Data4[1]), 4);
    buf[23] = '-';
    for (i = 2; i < 8; i++) hexn(buf + 24 + (i - 2) * 2, g->Data4[i], 2);
    buf[36] = 0;
    return buf;
}

static int guid_eq(const GUID *a, const GUID *b)
{
    return a->Data1 == b->Data1 && a->Data2 == b->Data2 && a->Data3 == b->Data3 &&
           memcmp(a->Data4, b->Data4, 8) == 0;
}

/* ------------------------------------------------------------------------ */
/* video window discovery                                                    */

typedef struct {
    DWORD pid;
    HWND  best;
    long  best_area;
} find_ctx_t;

static BOOL CALLBACK find_video_wnd_proc(HWND hwnd, LPARAM lp)
{
    find_ctx_t *ctx = (find_ctx_t *)lp;
    DWORD pid = 0;
    RECT rc;
    long w, h, area;
    char cls[96];

    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != ctx->pid) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    /* the engine's main window has no owner; wcphr.dll's video popup does */
    if (GetWindow(hwnd, GW_OWNER) == NULL) return TRUE;
    if (!GetWindowRect(hwnd, &rc)) return TRUE;
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    if (w < 64 || h < 64) return TRUE;   /* skip tool windows */
    if (!ctx->best) {   /* only log candidates while still searching */
        cls[0] = 0;
        GetClassNameA(hwnd, cls, (int)sizeof(cls));
        shim_log("EnumWindows: candidate hwnd=%p class=\"%s\" %ldx%ld",
                 (void *)hwnd, cls, w, h);
    }
    area = w * h;
    if (area > ctx->best_area) { ctx->best_area = area; ctx->best = hwnd; }
    return TRUE;
}

static HWND  g_video_wnd;
static DWORD g_video_wnd_check;

static HWND find_video_window(void)
{
    DWORD now = GetTickCount();
    find_ctx_t ctx;

    if (g_video_wnd && IsWindow(g_video_wnd) && IsWindowVisible(g_video_wnd) &&
        now - g_video_wnd_check < 1000)
        return g_video_wnd;

    ctx.pid = GetCurrentProcessId();
    ctx.best = NULL;
    ctx.best_area = 0;
    EnumWindows(find_video_wnd_proc, (LPARAM)&ctx);
    g_video_wnd_check = now;
    if (ctx.best != g_video_wnd) {
        if (ctx.best) shim_log("video window: using hwnd=%p", (void *)ctx.best);
        else          shim_log("video window: no owned visible window found (yet)");
    }
    g_video_wnd = ctx.best;
    return g_video_wnd;
}

/* fallback only: stays invisible under KWin/gamescope, but better than nothing */
static HWND g_fallback_wnd;

static HWND get_fallback_window(void)
{
    static const char *CLS = "WCPDDrawShimVideo";
    WNDCLASSA wc;

    if (g_fallback_wnd && IsWindow(g_fallback_wnd)) return g_fallback_wnd;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc   = DefWindowProcA;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.lpszClassName = CLS;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassA(&wc);
    g_fallback_wnd = CreateWindowExA(WS_EX_TOPMOST | WS_EX_NOACTIVATE, CLS, "WCP Video",
        WS_POPUP | WS_VISIBLE, 0, 0,
        GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
        NULL, NULL, wc.hInstance, NULL);
    if (g_fallback_wnd) {
        ShowWindow(g_fallback_wnd, SW_SHOWNOACTIVATE);
        shim_log("fallback window created: %p", (void *)g_fallback_wnd);
    } else {
        shim_log("fallback window creation FAILED, err=%lu", (unsigned long)GetLastError());
    }
    return g_fallback_wnd;
}

/* ------------------------------------------------------------------------ */
/* surface object                                                            */

typedef struct shim_dd      shim_dd_t;
typedef struct shim_surface shim_surface_t;

struct shim_surface {
    IDirectDrawSurfaceVtbl *lpVtbl;   /* must be first */
    LONG            ref;
    shim_dd_t      *dd;
    DWORD           caps;
    DWORD           width, height;
    LONG            pitch;
    BYTE           *pixels;
    int             owns_pixels;
    int             is_primary;
    shim_surface_t **backbuffers;
    DWORD           backbuffer_count;
    /* GetDC state */
    HDC             dc;
    HBITMAP         dib;
    void           *dib_bits;
    HGDIOBJ         dib_old;
};

struct shim_dd {
    IDirectDrawVtbl *lpVtbl;          /* must be first */
    LONG             ref;
    HWND             coop_hwnd;
    DWORD            coop_flags;
    shim_surface_t  *primary;
};

static void fill_pixel_format(DDPIXELFORMAT *pf)
{
    memset(pf, 0, sizeof *pf);
    pf->dwSize             = sizeof *pf;
    pf->dwFlags            = DDPF_RGB;
    pf->dwRGBBitCount      = 32;
    pf->dwRBitMask         = 0x00FF0000;
    pf->dwGBitMask         = 0x0000FF00;
    pf->dwBBitMask         = 0x000000FF;
    pf->dwRGBAlphaBitMask  = 0x00000000;
}

static void present_frame(shim_surface_t *s)
{
    HWND hwnd = find_video_window();
    HDC hdc;
    BITMAPINFO bmi;
    RECT rc;
    int cw, ch, r;

    if (!hwnd) hwnd = get_fallback_window();
    if (!hwnd) return;

    hdc = GetDC(hwnd);
    if (!hdc) { shim_log("present: GetDC(%p) failed", (void *)hwnd); return; }

    memset(&bmi, 0, sizeof bmi);
    bmi.bmiHeader.biSize        = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth       = (LONG)s->width;
    bmi.bmiHeader.biHeight      = -(LONG)s->height;   /* negative: top-down */
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    GetClientRect(hwnd, &rc);
    cw = rc.right - rc.left;
    ch = rc.bottom - rc.top;

    if (cw == (int)s->width && ch == (int)s->height) {
        r = SetDIBitsToDevice(hdc, 0, 0, s->width, s->height, 0, 0, 0, s->height,
                              s->pixels, &bmi, DIB_RGB_COLORS);
    } else {
        SetStretchBltMode(hdc, HALFTONE);
        r = StretchDIBits(hdc, 0, 0, cw, ch, 0, 0, (int)s->width, (int)s->height,
                          s->pixels, &bmi, DIB_RGB_COLORS, SRCCOPY);
    }
    if (log_maybe(&g_n_present))
        shim_log("present: hwnd=%p surf=%lux%lu client=%dx%d result=%d",
                 (void *)hwnd, (unsigned long)s->width, (unsigned long)s->height,
                 cw, ch, r);
    if (r == 0 || r == (int)GDI_ERROR)
        shim_log("present: GDI call FAILED err=%lu", (unsigned long)GetLastError());
    ReleaseDC(hwnd, hdc);
}

static void copy_rect(shim_surface_t *dst, const RECT *drc_in,
                      shim_surface_t *src, const RECT *src_in)
{
    RECT d, sr;
    LONG w, h, y;

    if (drc_in) d = *drc_in;
    else { d.left = 0; d.top = 0; d.right = (LONG)dst->width; d.bottom = (LONG)dst->height; }
    if (src_in) sr = *src_in;
    else { sr.left = 0; sr.top = 0; sr.right = (LONG)src->width; sr.bottom = (LONG)src->height; }

    w = d.right - d.left;
    h = d.bottom - d.top;
    if (sr.right - sr.left < w) w = sr.right - sr.left;
    if (sr.bottom - sr.top < h) h = sr.bottom - sr.top;
    if (d.left < 0) { sr.left -= d.left; w += d.left; d.left = 0; }
    if (d.top < 0)  { sr.top  -= d.top;  h += d.top;  d.top  = 0; }
    if (d.left + w > (LONG)dst->width)  w = (LONG)dst->width  - d.left;
    if (d.top  + h > (LONG)dst->height) h = (LONG)dst->height - d.top;
    if (sr.left < 0) { d.left -= sr.left; w += sr.left; sr.left = 0; }
    if (sr.top  < 0) { d.top  -= sr.top;  h += sr.top;  sr.top  = 0; }
    if (sr.left + w > (LONG)src->width)  w = (LONG)src->width  - sr.left;
    if (sr.top  + h > (LONG)src->height) h = (LONG)src->height - sr.top;
    if (w <= 0 || h <= 0) return;

    for (y = 0; y < h; y++)
        memcpy(dst->pixels + (size_t)(d.top + y) * (size_t)dst->pitch + (size_t)d.left * 4,
               src->pixels + (size_t)(sr.top + y) * (size_t)src->pitch + (size_t)sr.left * 4,
               (size_t)w * 4);
}

static void fill_rect(shim_surface_t *dst, const RECT *drc_in, DWORD color)
{
    RECT d;
    LONG x, y;

    if (drc_in) d = *drc_in;
    else { d.left = 0; d.top = 0; d.right = (LONG)dst->width; d.bottom = (LONG)dst->height; }
    if (d.left < 0) d.left = 0;
    if (d.top < 0)  d.top  = 0;
    if (d.right  > (LONG)dst->width)  d.right  = (LONG)dst->width;
    if (d.bottom > (LONG)dst->height) d.bottom = (LONG)dst->height;

    for (y = d.top; y < d.bottom; y++) {
        DWORD *row = (DWORD *)(dst->pixels + (size_t)y * (size_t)dst->pitch);
        for (x = d.left; x < d.right; x++) row[x] = color;
    }
}

/* --- IDirectDrawSurface v1 methods -------------------------------------- */

static HRESULT STDMETHODCALLTYPE S_QueryInterface(IDirectDrawSurface *This, REFIID riid, void **ppvObject)
{
    char buf[40];
    if (!ppvObject) return E_POINTER;
    if (guid_eq(riid, &IID_IUnknown) || guid_eq(riid, &IID_IDirectDrawSurface)) {
        InterlockedIncrement(&((shim_surface_t *)This)->ref);
        *ppvObject = This;
        return S_OK;
    }
    shim_log("surface QueryInterface: unsupported %s -> E_NOINTERFACE", guid_str(riid, buf));
    *ppvObject = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE S_AddRef(IDirectDrawSurface *This)
{
    return (ULONG)InterlockedIncrement(&((shim_surface_t *)This)->ref);
}

static ULONG STDMETHODCALLTYPE S_Release(IDirectDrawSurface *This)
{
    shim_surface_t *s = (shim_surface_t *)This;
    LONG r = InterlockedDecrement(&s->ref);
    if (!r) {
        DWORD i;
        shim_log("surface %p destroyed (%lux%lu primary=%d)",
                 (void *)s, (unsigned long)s->width, (unsigned long)s->height, s->is_primary);
        if (s->dc) {
            SelectObject(s->dc, s->dib_old);
            if (s->dib) DeleteObject(s->dib);
            DeleteDC(s->dc);
        }
        for (i = 0; i < s->backbuffer_count; i++)
            if (s->backbuffers[i]) S_Release((IDirectDrawSurface *)s->backbuffers[i]);
        xfree(s->backbuffers);
        if (s->owns_pixels) xfree(s->pixels);
        if (s->dd && s->dd->primary == s) s->dd->primary = NULL;
        xfree(s);
    }
    return (ULONG)r;
}

static HRESULT STDMETHODCALLTYPE S_AddAttachedSurface(IDirectDrawSurface *This, LPDIRECTDRAWSURFACE lpDDSurface)
{
    shim_log("surface AddAttachedSurface: %p <- %p (ignored)", (void *)This, (void *)lpDDSurface);
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE S_AddOverlayDirtyRect(IDirectDrawSurface *This, LPRECT lpRect)
{
    shim_log("surface AddOverlayDirtyRect: %p", (void *)This);
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE S_Blt(IDirectDrawSurface *This, LPRECT lpDestRect, LPDIRECTDRAWSURFACE lpDDSrcSurface, LPRECT lpSrcRect, DWORD dwFlags, LPDDBLTFX lpDDBltFx)
{
    shim_surface_t *dst = (shim_surface_t *)This;
    shim_surface_t *src = (shim_surface_t *)lpDDSrcSurface;

    if (log_maybe(&g_n_blt))
        shim_log("surface Blt: dst=%p src=%p flags=0x%08lx", (void *)dst, (void *)src, dwFlags);

    if ((dwFlags & DDBLT_COLORFILL) && lpDDBltFx) {
        fill_rect(dst, lpDestRect, lpDDBltFx->dwFillColor);
    } else if (src && src->pixels) {
        copy_rect(dst, lpDestRect, src, lpSrcRect);
    }
    if (dst->is_primary || !dst->dd->primary) present_frame(dst);
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE S_BltBatch(IDirectDrawSurface *This, LPDDBLTBATCH lpDDBltBatch, DWORD dwCount, DWORD dwFlags)
{
    shim_log("surface BltBatch: %p count=%lu (ignored)", (void *)This, (unsigned long)dwCount);
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE S_BltFast(IDirectDrawSurface *This, DWORD dwX, DWORD dwY, LPDIRECTDRAWSURFACE lpDDSrcSurface, LPRECT lpSrcRect, DWORD dwTrans)
{
    shim_surface_t *dst = (shim_surface_t *)This;
    shim_surface_t *src = (shim_surface_t *)lpDDSrcSurface;

    if (log_maybe(&g_n_bltfast))
        shim_log("surface BltFast: dst=%p src=%p xy=%lu,%lu", (void *)dst, (void *)src,
                 (unsigned long)dwX, (unsigned long)dwY);

    if (src && src->pixels) {
        RECT d;
        d.left   = (LONG)dwX;
        d.top    = (LONG)dwY;
        d.right  = (LONG)dwX + (lpSrcRect ? (lpSrcRect->right - lpSrcRect->left) : (LONG)src->width);
        d.bottom = (LONG)dwY + (lpSrcRect ? (lpSrcRect->bottom - lpSrcRect->top) : (LONG)src->height);
        copy_rect(dst, &d, src, lpSrcRect);
    }
    if (dst->is_primary || !dst->dd->primary) present_frame(dst);
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE S_DeleteAttachedSurface(IDirectDrawSurface *This, DWORD dwFlags, LPDIRECTDRAWSURFACE lpDDSurface)
{
    shim_log("surface DeleteAttachedSurface: %p", (void *)This);
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE S_EnumAttachedSurfaces(IDirectDrawSurface *This, LPVOID lpContext, LPDDENUMSURFACESCALLBACK lpEnumSurfacesCallback)
{
    shim_surface_t *s = (shim_surface_t *)This;
    DWORD i;
    if (!lpEnumSurfacesCallback) return DDERR_INVALIDPARAMS;
    for (i = 0; i < s->backbuffer_count; i++) {
        if (s->backbuffers[i]) {
            DDSURFACEDESC d;
            memset(&d, 0, sizeof d);
            d.dwSize = sizeof d;
            d.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_CAPS;
            d.dwWidth  = s->backbuffers[i]->width;
            d.dwHeight = s->backbuffers[i]->height;
            d.lPitch   = s->backbuffers[i]->pitch;
            d.ddsCaps.dwCaps = s->backbuffers[i]->caps;
            fill_pixel_format(&d.ddpfPixelFormat);
            S_AddRef((IDirectDrawSurface *)s->backbuffers[i]);
            lpEnumSurfacesCallback((LPDIRECTDRAWSURFACE)s->backbuffers[i], &d, lpContext);
            S_Release((IDirectDrawSurface *)s->backbuffers[i]);
        }
    }
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE S_EnumOverlayZOrders(IDirectDrawSurface *This, DWORD dwFlags, LPVOID lpContext, LPDDENUMSURFACESCALLBACK lpEnumSurfacesCallback)
{
    shim_log("surface EnumOverlayZOrders: %p", (void *)This);
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE S_Flip(IDirectDrawSurface *This, LPDIRECTDRAWSURFACE lpDDSurfaceTargetOverride, DWORD dwFlags)
{
    shim_surface_t *s = (shim_surface_t *)This;

    if (log_maybe(&g_n_flip))
        shim_log("surface Flip: %p override=%p backbuffers=%lu",
                 (void *)s, (void *)lpDDSurfaceTargetOverride,
                 (unsigned long)s->backbuffer_count);

    if (s->backbuffer_count && s->backbuffers[0]) {
        /* swap front buffer with first back buffer */
        BYTE *tmp = s->pixels;
        s->pixels = s->backbuffers[0]->pixels;
        s->backbuffers[0]->pixels = tmp;
    }
    present_frame(s);
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE S_GetAttachedSurface(IDirectDrawSurface *This, LPDDSCAPS lpDDSCaps, LPDIRECTDRAWSURFACE *lplpDDAttachedSurface)
{
    shim_surface_t *s = (shim_surface_t *)This;
    if (!lplpDDAttachedSurface) return DDERR_INVALIDPARAMS;
    if (s->backbuffer_count && lpDDSCaps && (lpDDSCaps->dwCaps & DDSCAPS_BACKBUFFER)) {
        *lplpDDAttachedSurface = (LPDIRECTDRAWSURFACE)s->backbuffers[0];
        S_AddRef(*lplpDDAttachedSurface);
        return DD_OK;
    }
    *lplpDDAttachedSurface = NULL;
    return DDERR_NOTFOUND;
}

static HRESULT STDMETHODCALLTYPE S_GetBltStatus(IDirectDrawSurface *This, DWORD dwFlags)
{
    return DD_OK;   /* never busy */
}

static HRESULT STDMETHODCALLTYPE S_GetCaps(IDirectDrawSurface *This, LPDDSCAPS lpDDSCaps)
{
    shim_surface_t *s = (shim_surface_t *)This;
    if (!lpDDSCaps) return DDERR_INVALIDPARAMS;
    lpDDSCaps->dwCaps = s->caps;
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE S_GetClipper(IDirectDrawSurface *This, LPDIRECTDRAWCLIPPER *lplpDDClipper)
{
    if (lplpDDClipper) *lplpDDClipper = NULL;
    return DDERR_NOCLIPPERATTACHED;
}

static HRESULT STDMETHODCALLTYPE S_GetColorKey(IDirectDrawSurface *This, DWORD dwFlags, LPDDCOLORKEY lpDDColorKey)
{
    return DDERR_NOCOLORKEY;
}

static HRESULT STDMETHODCALLTYPE S_GetDC(IDirectDrawSurface *This, HDC *lphDC)
{
    shim_surface_t *s = (shim_surface_t *)This;
    BITMAPINFO bmi;

    shim_log("surface GetDC: %p", (void *)s);
    if (!lphDC) return DDERR_INVALIDPARAMS;
    if (s->dc) { *lphDC = s->dc; return DD_OK; }

    memset(&bmi, 0, sizeof bmi);
    bmi.bmiHeader.biSize        = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth       = (LONG)s->width;
    bmi.bmiHeader.biHeight      = -(LONG)s->height;
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    s->dc  = CreateCompatibleDC(NULL);
    s->dib = CreateDIBSection(s->dc, &bmi, DIB_RGB_COLORS, &s->dib_bits, NULL, 0);
    if (!s->dc || !s->dib) {
        if (s->dib) DeleteObject(s->dib);
        if (s->dc)  DeleteDC(s->dc);
        s->dc = NULL; s->dib = NULL;
        *lphDC = NULL;
        shim_log("surface GetDC: FAILED");
        return DDERR_GENERIC;
    }
    memcpy(s->dib_bits, s->pixels, (size_t)s->pitch * (size_t)s->height);
    s->dib_old = SelectObject(s->dc, s->dib);
    *lphDC = s->dc;
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE S_GetFlipStatus(IDirectDrawSurface *This, DWORD dwFlags)
{
    return DD_OK;   /* never flipping */
}

static HRESULT STDMETHODCALLTYPE S_GetOverlayPosition(IDirectDrawSurface *This, LPLONG lplX, LPLONG lplY)
{
    if (lplX) *lplX = 0;
    if (lplY) *lplY = 0;
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE S_GetPalette(IDirectDrawSurface *This, LPDIRECTDRAWPALETTE *lplpDDPalette)
{
    if (lplpDDPalette) *lplpDDPalette = NULL;
    return DDERR_NOPALETTEATTACHED;
}

static HRESULT STDMETHODCALLTYPE S_GetPixelFormat(IDirectDrawSurface *This, LPDDPIXELFORMAT lpDDPixelFormat)
{
    if (!lpDDPixelFormat) return DDERR_INVALIDPARAMS;
    fill_pixel_format(lpDDPixelFormat);
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE S_GetSurfaceDesc(IDirectDrawSurface *This, LPDDSURFACEDESC lpDDSurfaceDesc)
{
    shim_surface_t *s = (shim_surface_t *)This;
    if (!lpDDSurfaceDesc) return DDERR_INVALIDPARAMS;
    lpDDSurfaceDesc->dwFlags  = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_CAPS;
    lpDDSurfaceDesc->dwWidth  = s->width;
    lpDDSurfaceDesc->dwHeight = s->height;
    lpDDSurfaceDesc->lPitch   = s->pitch;
    lpDDSurfaceDesc->ddsCaps.dwCaps = s->caps;
    fill_pixel_format(&lpDDSurfaceDesc->ddpfPixelFormat);
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE S_Initialize(IDirectDrawSurface *This, LPDIRECTDRAW lpDD, LPDDSURFACEDESC lpDDSurfaceDesc)
{
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE S_IsLost(IDirectDrawSurface *This)
{
    return DD_OK;   /* never lost */
}

static HRESULT STDMETHODCALLTYPE S_Lock(IDirectDrawSurface *This, LPRECT lpDestRect, LPDDSURFACEDESC lpDDSurfaceDesc, DWORD dwFlags, HANDLE hEvent)
{
    shim_surface_t *s = (shim_surface_t *)This;
    BYTE *ptr = s->pixels;

    if (log_maybe(&g_n_lock)) {
        if (lpDestRect)
            shim_log("surface Lock: %p rect=%ld,%ld-%ld,%ld flags=0x%08lx",
                     (void *)s, (long)lpDestRect->left, (long)lpDestRect->top,
                     (long)lpDestRect->right, (long)lpDestRect->bottom, dwFlags);
        else
            shim_log("surface Lock: %p rect=(full) flags=0x%08lx", (void *)s, dwFlags);
    }

    if (!lpDDSurfaceDesc) return DDERR_INVALIDPARAMS;
    if (lpDestRect)
        ptr += (size_t)lpDestRect->top * (size_t)s->pitch + (size_t)lpDestRect->left * 4;

    lpDDSurfaceDesc->dwFlags  = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH |
                                DDSD_PIXELFORMAT | DDSD_CAPS | DDSD_LPSURFACE;
    lpDDSurfaceDesc->dwWidth  = s->width;
    lpDDSurfaceDesc->dwHeight = s->height;
    lpDDSurfaceDesc->lPitch   = s->pitch;
    lpDDSurfaceDesc->lpSurface = ptr;
    lpDDSurfaceDesc->ddsCaps.dwCaps = s->caps;
    fill_pixel_format(&lpDDSurfaceDesc->ddpfPixelFormat);
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE S_ReleaseDC(IDirectDrawSurface *This, HDC hDC)
{
    shim_surface_t *s = (shim_surface_t *)This;
    shim_log("surface ReleaseDC: %p", (void *)s);
    if (s->dc && hDC == s->dc) {
        memcpy(s->pixels, s->dib_bits, (size_t)s->pitch * (size_t)s->height);
        SelectObject(s->dc, s->dib_old);
        DeleteObject(s->dib);
        DeleteDC(s->dc);
        s->dc = NULL; s->dib = NULL; s->dib_bits = NULL;
        if (s->is_primary || !s->dd->primary) present_frame(s);
    }
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE S_Restore(IDirectDrawSurface *This)
{
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE S_SetClipper(IDirectDrawSurface *This, LPDIRECTDRAWCLIPPER lpDDClipper)
{
    shim_log("surface SetClipper: %p clipper=%p (ignored)", (void *)This, (void *)lpDDClipper);
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE S_SetColorKey(IDirectDrawSurface *This, DWORD dwFlags, LPDDCOLORKEY lpDDColorKey)
{
    shim_log("surface SetColorKey: %p flags=0x%08lx (ignored)", (void *)This, dwFlags);
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE S_SetOverlayPosition(IDirectDrawSurface *This, LONG lX, LONG lY)
{
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE S_SetPalette(IDirectDrawSurface *This, LPDIRECTDRAWPALETTE lpDDPalette)
{
    shim_log("surface SetPalette: %p (ignored)", (void *)This);
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE S_Unlock(IDirectDrawSurface *This, LPVOID lpSurfaceData)
{
    shim_surface_t *s = (shim_surface_t *)This;

    if (log_maybe(&g_n_unlock))
        shim_log("surface Unlock: %p data=%p", (void *)s, lpSurfaceData);

    if (s->is_primary || !s->dd->primary) present_frame(s);
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE S_UpdateOverlay(IDirectDrawSurface *This, LPRECT lpSrcRect, LPDIRECTDRAWSURFACE lpDDDestSurface, LPRECT lpDestRect, DWORD dwFlags, LPDDOVERLAYFX lpDDOverlayFx)
{
    shim_log("surface UpdateOverlay: %p (ignored)", (void *)This);
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE S_UpdateOverlayDisplay(IDirectDrawSurface *This, DWORD dwFlags)
{
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE S_UpdateOverlayZOrder(IDirectDrawSurface *This, DWORD dwFlags, LPDIRECTDRAWSURFACE lpDDSReference)
{
    return DD_OK;
}

static const IDirectDrawSurfaceVtbl surface_vtbl = {
    .QueryInterface        = S_QueryInterface,
    .AddRef                = S_AddRef,
    .Release               = S_Release,
    .AddAttachedSurface    = S_AddAttachedSurface,
    .AddOverlayDirtyRect   = S_AddOverlayDirtyRect,
    .Blt                   = S_Blt,
    .BltBatch              = S_BltBatch,
    .BltFast               = S_BltFast,
    .DeleteAttachedSurface = S_DeleteAttachedSurface,
    .EnumAttachedSurfaces  = S_EnumAttachedSurfaces,
    .EnumOverlayZOrders    = S_EnumOverlayZOrders,
    .Flip                  = S_Flip,
    .GetAttachedSurface    = S_GetAttachedSurface,
    .GetBltStatus          = S_GetBltStatus,
    .GetCaps               = S_GetCaps,
    .GetClipper            = S_GetClipper,
    .GetColorKey           = S_GetColorKey,
    .GetDC                 = S_GetDC,
    .GetFlipStatus         = S_GetFlipStatus,
    .GetOverlayPosition    = S_GetOverlayPosition,
    .GetPalette            = S_GetPalette,
    .GetPixelFormat        = S_GetPixelFormat,
    .GetSurfaceDesc        = S_GetSurfaceDesc,
    .Initialize            = S_Initialize,
    .IsLost                = S_IsLost,
    .Lock                  = S_Lock,
    .ReleaseDC             = S_ReleaseDC,
    .Restore               = S_Restore,
    .SetClipper            = S_SetClipper,
    .SetColorKey           = S_SetColorKey,
    .SetOverlayPosition    = S_SetOverlayPosition,
    .SetPalette            = S_SetPalette,
    .Unlock                = S_Unlock,
    .UpdateOverlay         = S_UpdateOverlay,
    .UpdateOverlayDisplay  = S_UpdateOverlayDisplay,
    .UpdateOverlayZOrder   = S_UpdateOverlayZOrder,
};

static shim_surface_t *surface_create(shim_dd_t *dd, DWORD caps, DWORD w, DWORD h, int is_primary)
{
    shim_surface_t *s = (shim_surface_t *)xalloc(sizeof(*s));
    if (!s) return NULL;
    s->lpVtbl      = (IDirectDrawSurfaceVtbl *)&surface_vtbl;
    s->ref         = 1;
    s->dd          = dd;
    s->caps        = caps;
    s->width       = w;
    s->height      = h;
    s->pitch       = (LONG)(w * 4);
    s->is_primary  = is_primary;
    s->owns_pixels = 1;
    s->pixels      = (BYTE *)xalloc((size_t)s->pitch * (size_t)h);
    if (!s->pixels) { xfree(s); return NULL; }
    return s;
}

/* --- IDirectDraw v1 methods --------------------------------------------- */

static HRESULT STDMETHODCALLTYPE DD_QueryInterface(IDirectDraw *This, REFIID riid, void **ppvObject)
{
    char buf[40];
    if (!ppvObject) return E_POINTER;
    if (guid_eq(riid, &IID_IUnknown) || guid_eq(riid, &IID_IDirectDraw)) {
        InterlockedIncrement(&((shim_dd_t *)This)->ref);
        *ppvObject = This;
        return S_OK;
    }
    shim_log("dd QueryInterface: unsupported %s -> E_NOINTERFACE", guid_str(riid, buf));
    *ppvObject = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE DD_AddRef(IDirectDraw *This)
{
    return (ULONG)InterlockedIncrement(&((shim_dd_t *)This)->ref);
}

static ULONG STDMETHODCALLTYPE DD_Release(IDirectDraw *This)
{
    shim_dd_t *dd = (shim_dd_t *)This;
    LONG r = InterlockedDecrement(&dd->ref);
    if (!r) {
        shim_log("DirectDraw object %p destroyed", (void *)dd);
        xfree(dd);
    }
    return (ULONG)r;
}

static HRESULT STDMETHODCALLTYPE DD_Compact(IDirectDraw *This)
{
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE DD_CreateClipper(IDirectDraw *This, DWORD dwFlags, LPDIRECTDRAWCLIPPER *lplpDDClipper, IUnknown *pUnkOuter)
{
    shim_log("dd CreateClipper (returning NULL)");
    if (lplpDDClipper) *lplpDDClipper = NULL;
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE DD_CreatePalette(IDirectDraw *This, DWORD dwFlags, LPPALETTEENTRY lpColorTable, LPDIRECTDRAWPALETTE *lplpDDPalette, IUnknown *pUnkOuter)
{
    shim_log("dd CreatePalette (returning NULL)");
    if (lplpDDPalette) *lplpDDPalette = NULL;
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE DD_CreateSurface(IDirectDraw *This, LPDDSURFACEDESC lpDDSurfaceDesc, LPDIRECTDRAWSURFACE *lplpDDSurface, IUnknown *pUnkOuter)
{
    shim_dd_t *dd = (shim_dd_t *)This;
    DWORD flags, caps, w, h, bbc, i;
    shim_surface_t *s;

    if (!lpDDSurfaceDesc || !lplpDDSurface) return DDERR_INVALIDPARAMS;

    flags = lpDDSurfaceDesc->dwFlags;
    caps  = (flags & DDSD_CAPS)           ? lpDDSurfaceDesc->ddsCaps.dwCaps    : 0;
    w     = (flags & DDSD_WIDTH)          ? lpDDSurfaceDesc->dwWidth           : 0;
    h     = (flags & DDSD_HEIGHT)         ? lpDDSurfaceDesc->dwHeight          : 0;
    bbc   = (flags & DDSD_BACKBUFFERCOUNT)? lpDDSurfaceDesc->dwBackBufferCount : 0;

    shim_log("CreateSurface: flags=0x%08lx caps=0x%08lx %lux%lu backbuffers=%lu",
             flags, caps, (unsigned long)w, (unsigned long)h, (unsigned long)bbc);

    if (caps & DDSCAPS_PRIMARYSURFACE) {
        if (!w) w = (DWORD)GetSystemMetrics(SM_CXSCREEN);
        if (!h) h = (DWORD)GetSystemMetrics(SM_CYSCREEN);
    }
    if (!w) w = (DWORD)GetSystemMetrics(SM_CXSCREEN);
    if (!h) h = (DWORD)GetSystemMetrics(SM_CYSCREEN);

    s = surface_create(dd, caps, w, h, (caps & DDSCAPS_PRIMARYSURFACE) != 0);
    if (!s) return DDERR_OUTOFMEMORY;

    if (s->is_primary && !dd->primary) dd->primary = s;

    if (bbc && s->is_primary) {
        s->backbuffers = (shim_surface_t **)xalloc((size_t)bbc * sizeof(shim_surface_t *));
        s->backbuffer_count = bbc;
        for (i = 0; i < bbc; i++) {
            s->backbuffers[i] = surface_create(dd, DDSCAPS_BACKBUFFER, w, h, 0);
            if (!s->backbuffers[i]) shim_log("CreateSurface: backbuffer %lu FAILED", (unsigned long)i);
        }
    }

    shim_log("CreateSurface: -> %p %lux%lu pitch=%ld primary=%d",
             (void *)s, (unsigned long)w, (unsigned long)h, (long)s->pitch, s->is_primary);

    *lplpDDSurface = (LPDIRECTDRAWSURFACE)s;
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE DD_DuplicateSurface(IDirectDraw *This, LPDIRECTDRAWSURFACE lpDDSurface, LPDIRECTDRAWSURFACE *lplpDupDDSurface)
{
    shim_surface_t *src = (shim_surface_t *)lpDDSurface;
    shim_surface_t *s;

    if (!src || !lplpDupDDSurface) return DDERR_INVALIDPARAMS;
    s = (shim_surface_t *)xalloc(sizeof(*s));
    if (!s) return DDERR_OUTOFMEMORY;
    memcpy(s, src, sizeof(*s));
    s->ref              = 1;
    s->owns_pixels      = 0;   /* shared with the original */
    s->backbuffers      = NULL;
    s->backbuffer_count = 0;
    s->dc = NULL; s->dib = NULL; s->dib_bits = NULL;
    *lplpDupDDSurface = (LPDIRECTDRAWSURFACE)s;
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE DD_EnumDisplayModes(IDirectDraw *This, DWORD dwFlags, LPDDSURFACEDESC lpDDSurfaceDesc, LPVOID lpContext, LPDDENUMMODESCALLBACK lpEnumModesCallback)
{
    DDSURFACEDESC d;
    shim_log("dd EnumDisplayModes flags=0x%08lx", dwFlags);
    if (!lpEnumModesCallback) return DDERR_INVALIDPARAMS;
    memset(&d, 0, sizeof d);
    d.dwSize  = sizeof d;
    d.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
    d.dwWidth  = (DWORD)GetSystemMetrics(SM_CXSCREEN);
    d.dwHeight = (DWORD)GetSystemMetrics(SM_CYSCREEN);
    d.dwRefreshRate = 60;
    fill_pixel_format(&d.ddpfPixelFormat);
    lpEnumModesCallback(&d, lpContext);
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE DD_EnumSurfaces(IDirectDraw *This, DWORD dwFlags, LPDDSURFACEDESC lpDDSD, LPVOID lpContext, LPDDENUMSURFACESCALLBACK lpEnumSurfacesCallback)
{
    shim_dd_t *dd = (shim_dd_t *)This;
    DDSURFACEDESC d;
    if (!lpEnumSurfacesCallback) return DDERR_INVALIDPARAMS;
    if (dd->primary) {
        memset(&d, 0, sizeof d);
        d.dwSize = sizeof d;
        S_GetSurfaceDesc((IDirectDrawSurface *)dd->primary, &d);
        S_AddRef((IDirectDrawSurface *)dd->primary);
        lpEnumSurfacesCallback((LPDIRECTDRAWSURFACE)dd->primary, &d, lpContext);
        S_Release((IDirectDrawSurface *)dd->primary);
    }
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE DD_FlipToGDISurface(IDirectDraw *This)
{
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE DD_GetCaps(IDirectDraw *This, LPDDCAPS lpDDDriverCaps, LPDDCAPS lpDDHELCaps)
{
    shim_log("dd GetCaps");
    if (lpDDDriverCaps) {
        DWORD sz = lpDDDriverCaps->dwSize;
        memset(lpDDDriverCaps, 0, sz);
        lpDDDriverCaps->dwSize = sz;
    }
    if (lpDDHELCaps) {
        DWORD sz = lpDDHELCaps->dwSize;
        memset(lpDDHELCaps, 0, sz);
        lpDDHELCaps->dwSize = sz;
    }
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE DD_GetDisplayMode(IDirectDraw *This, LPDDSURFACEDESC lpDDSurfaceDesc)
{
    if (!lpDDSurfaceDesc) return DDERR_INVALIDPARAMS;
    lpDDSurfaceDesc->dwFlags  = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
    lpDDSurfaceDesc->dwWidth  = (DWORD)GetSystemMetrics(SM_CXSCREEN);
    lpDDSurfaceDesc->dwHeight = (DWORD)GetSystemMetrics(SM_CYSCREEN);
    lpDDSurfaceDesc->dwRefreshRate = 60;
    fill_pixel_format(&lpDDSurfaceDesc->ddpfPixelFormat);
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE DD_GetFourCCCodes(IDirectDraw *This, LPDWORD lpNumCodes, LPDWORD lpCodes)
{
    if (lpNumCodes) *lpNumCodes = 0;
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE DD_GetGDISurface(IDirectDraw *This, LPDIRECTDRAWSURFACE *lplpGDIDDSurface)
{
    if (lplpGDIDDSurface) *lplpGDIDDSurface = NULL;
    return DDERR_NOTFOUND;
}

static HRESULT STDMETHODCALLTYPE DD_GetMonitorFrequency(IDirectDraw *This, LPDWORD lpdwFrequency)
{
    if (lpdwFrequency) *lpdwFrequency = 60;
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE DD_GetScanLine(IDirectDraw *This, LPDWORD lpdwScanLine)
{
    if (lpdwScanLine) *lpdwScanLine = 0;
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE DD_GetVerticalBlankStatus(IDirectDraw *This, LPBOOL lpbIsInVB)
{
    if (lpbIsInVB) *lpbIsInVB = TRUE;
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE DD_Initialize(IDirectDraw *This, GUID *lpGUID)
{
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE DD_RestoreDisplayMode(IDirectDraw *This)
{
    shim_log("dd RestoreDisplayMode");
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE DD_SetCooperativeLevel(IDirectDraw *This, HWND hWnd, DWORD dwFlags)
{
    shim_dd_t *dd = (shim_dd_t *)This;
    dd->coop_hwnd  = hWnd;
    dd->coop_flags = dwFlags;
    shim_log("SetCooperativeLevel: hwnd=%p flags=0x%08lx%s",
             (void *)hWnd, dwFlags, (dwFlags & DDSCL_FULLSCREEN) ? " FULLSCREEN" : " (normal)");
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE DD_SetDisplayMode(IDirectDraw *This, DWORD dwWidth, DWORD dwHeight, DWORD dwBPP)
{
    shim_log("dd SetDisplayMode: %lux%lux%lu (ignored, staying windowed)",
             (unsigned long)dwWidth, (unsigned long)dwHeight, (unsigned long)dwBPP);
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE DD_WaitForVerticalBlank(IDirectDraw *This, DWORD dwFlags, HANDLE hEvent)
{
    return DD_OK;
}

static const IDirectDrawVtbl dd_vtbl = {
    .QueryInterface         = DD_QueryInterface,
    .AddRef                 = DD_AddRef,
    .Release                = DD_Release,
    .Compact                = DD_Compact,
    .CreateClipper          = DD_CreateClipper,
    .CreatePalette          = DD_CreatePalette,
    .CreateSurface          = DD_CreateSurface,
    .DuplicateSurface       = DD_DuplicateSurface,
    .EnumDisplayModes       = DD_EnumDisplayModes,
    .EnumSurfaces           = DD_EnumSurfaces,
    .FlipToGDISurface       = DD_FlipToGDISurface,
    .GetCaps                = DD_GetCaps,
    .GetDisplayMode         = DD_GetDisplayMode,
    .GetFourCCCodes         = DD_GetFourCCCodes,
    .GetGDISurface          = DD_GetGDISurface,
    .GetMonitorFrequency    = DD_GetMonitorFrequency,
    .GetScanLine            = DD_GetScanLine,
    .GetVerticalBlankStatus = DD_GetVerticalBlankStatus,
    .Initialize             = DD_Initialize,
    .RestoreDisplayMode     = DD_RestoreDisplayMode,
    .SetCooperativeLevel    = DD_SetCooperativeLevel,
    .SetDisplayMode         = DD_SetDisplayMode,
    .WaitForVerticalBlank   = DD_WaitForVerticalBlank,
};

/* ------------------------------------------------------------------------ */
/* exported entry points (names come from ddraw.def)                         */

HRESULT WINAPI XDirectDrawCreate(GUID *lpGUID, LPDIRECTDRAW *lplpDD, IUnknown *pUnkOuter)
{
    shim_dd_t *dd;
    shim_log("DirectDrawCreate guid=%p", (void *)lpGUID);
    if (!lplpDD) return DDERR_INVALIDPARAMS;
    dd = (shim_dd_t *)xalloc(sizeof(*dd));
    if (!dd) return DDERR_OUTOFMEMORY;
    dd->lpVtbl = (IDirectDrawVtbl *)&dd_vtbl;
    dd->ref    = 1;
    *lplpDD = (LPDIRECTDRAW)dd;
    shim_log("DirectDrawCreate: -> %p", (void *)dd);
    return DD_OK;
}

HRESULT WINAPI XDirectDrawCreateEx(GUID *lpGUID, LPVOID *lplpDD, REFIID riid, IUnknown *pUnkOuter)
{
    char buf[40];
    shim_log("DirectDrawCreateEx iid=%s", guid_str(riid, buf));
    if (!lplpDD) return DDERR_INVALIDPARAMS;
    if (!riid || !guid_eq(riid, &IID_IDirectDraw)) {
        *lplpDD = NULL;
        shim_log("DirectDrawCreateEx: unsupported IID -> E_NOINTERFACE");
        return E_NOINTERFACE;
    }
    return XDirectDrawCreate(lpGUID, (LPDIRECTDRAW *)lplpDD, pUnkOuter);
}

HRESULT WINAPI XDirectDrawCreateClipper(DWORD dwFlags, LPDIRECTDRAWCLIPPER *lplpDDClipper, IUnknown *pUnkOuter)
{
    shim_log("DirectDrawCreateClipper (returning NULL)");
    if (lplpDDClipper) *lplpDDClipper = NULL;
    return DD_OK;
}

HRESULT WINAPI XDirectDrawEnumerateA(LPDDENUMCALLBACKA lpCallback, LPVOID lpContext)
{
    shim_log("DirectDrawEnumerateA");
    if (lpCallback) lpCallback(NULL, (LPSTR)"WCP DDraw Shim", (LPSTR)"display", lpContext);
    return DD_OK;
}

HRESULT WINAPI XDirectDrawEnumerateW(LPDDENUMCALLBACKW lpCallback, LPVOID lpContext)
{
    shim_log("DirectDrawEnumerateW");
    if (lpCallback) lpCallback(NULL, (LPWSTR)L"WCP DDraw Shim", (LPWSTR)L"display", lpContext);
    return DD_OK;
}

HRESULT WINAPI XDirectDrawEnumerateExA(LPDDENUMCALLBACKEXA lpCallback, LPVOID lpContext, DWORD dwFlags)
{
    shim_log("DirectDrawEnumerateExA flags=0x%08lx", dwFlags);
    if (lpCallback) lpCallback(NULL, (LPSTR)"WCP DDraw Shim", (LPSTR)"display", lpContext, NULL);
    return DD_OK;
}

HRESULT WINAPI XDirectDrawEnumerateExW(LPDDENUMCALLBACKEXW lpCallback, LPVOID lpContext, DWORD dwFlags)
{
    shim_log("DirectDrawEnumerateExW flags=0x%08lx", dwFlags);
    if (lpCallback) lpCallback(NULL, (LPWSTR)L"WCP DDraw Shim", (LPWSTR)L"display", lpContext, NULL);
    return DD_OK;
}

HRESULT WINAPI XDllCanUnloadNow(void)  { return S_FALSE; }
HRESULT WINAPI XDllRegisterServer(void) { return S_OK; }
HRESULT WINAPI XDllUnregisterServer(void) { return S_OK; }

HRESULT WINAPI XDllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID *ppv)
{
    shim_log("DllGetClassObject -> CLASS_E_CLASSNOTAVAILABLE");
    if (ppv) *ppv = NULL;
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI XGetDDSurfaceLocal(void *a, void *b, void *c) { return DDERR_UNSUPPORTED; }
HRESULT WINAPI XGetSurfaceFromDC(HDC hdc, void *a, void *b)  { return DDERR_NOTFOUND; }
HRESULT WINAPI XRegisterSpecialCase(void)                    { return S_OK; }
HRESULT WINAPI XReleaseDDThreadLock(void)                    { return S_OK; }

/* ------------------------------------------------------------------------ */

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        InitializeCriticalSection(&g_cs);
        DisableThreadLibraryCalls(hinst);
        /* first log line proves the shim was loaded at all (see WINEDLLOVERRIDES) */
        shim_log("=== ddraw shim loaded (WCP Enhanced FMV fix) pid=%lu ===",
                 (unsigned long)GetCurrentProcessId());
    } else if (reason == DLL_PROCESS_DETACH) {
        shim_log("=== ddraw shim unloaded ===");
        if (g_hlog) { CloseHandle(g_hlog); g_hlog = NULL; }
        DeleteCriticalSection(&g_cs);
    }
    return TRUE;
}
