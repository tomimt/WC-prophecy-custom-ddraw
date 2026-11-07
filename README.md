# Custom ddraw.dll for Wing Commander Prophecy: 30th Anniversary Edition (Linux/Proton FMV fix)

A replacement 32-bit `ddraw.dll` that fixes the upscaled/mastered FMV cutscenes
in **Wing Commander Prophecy: 30th Anniversary Edition** (Steam AppID 3067120,
"Enhanced" build) when running under **Linux / Proton / Wine** — including
Steam Deck. Without it, the MP4 cutscenes play **audio with a black or white
screen**.

The original (non-upscaled) FMVs are unaffected by this bug and by this shim:
they are `movie\NNNN.wve` files inside `movies.tre`, decoded and drawn by the
game engine itself in OpenGL — a completely different code path.

## Why the videos are broken

The Enhanced build plays the MP4 cutscenes through `dshow.dll`, a
self-contained DirectShow reimplementation with ffmpeg built in. Its video
renderer presents frames the 1997 way:

```
DirectDrawCreate(NULL)
SetCooperativeLevel(hwnd = NULL, DDSCL_NORMAL)        <-- no window
CreateSurface(DDSCAPS_PRIMARYSURFACE, desktop resolution, RGB32)
per frame: Lock -> memcpy decoded frame -> Unlock
```

A primary surface with no cooperative-level window means "write into the
visible framebuffer". Under Wine there is none, so Wine's builtin ddraw blits
the surface onto the desktop device context (`GetDCEx(NULL)`) ~30 times a
second — which never reaches the screen under any modern compositor (KWin,
gamescope, ...). Audio runs through a separate DirectShow filter, hence sound
without picture.

Decoding is **not** the problem: DXVA2/D3D11 are never loaded, ffmpeg decodes
in software correctly, and the MP4s play fine in any media player.

## What the shim does

Placed **only** in the game's `Enhanced/` folder (nothing else there uses
DirectDraw, so the blast radius is one folder), the shim:

- Implements `IDirectDraw` v1 and `IDirectDrawSurface` v1 far enough for the
  call sequence above: `CreateSurface` serves the primary surface as an
  ordinary top-down BGRX memory buffer; `Lock` hands out that buffer and fills
  `DDSURFACEDESC`; `Unlock` presents it.
- Presents with `SetDIBitsToDevice` (negative `biHeight` = top-down), falling
  back to `StretchDIBits` if the window size differs from the surface.
- Also implements `Blt` / `BltFast` / `Flip` (including back-buffer chains and
  color-fill) and `GetDC` / `ReleaseDC`, in case the renderer takes that route.
- Draws into the video popup window the game already created, found via
  `EnumWindows`: same process, `IsWindowVisible()`, and
  `GetWindow(h, GW_OWNER) != NULL` — the engine's main window has no owner,
  `wcphr.dll`'s video popup does. (Presenting into an own
  `WS_EX_TOPMOST | WS_EX_NOACTIVATE` window does **not** work: KWin/gamescope
  keeps the game's active fullscreen window above it. An own window is kept
  only as a last-resort fallback.)
- Writes `ddraw_shim.log` next to the game executable for diagnosis.
- Depends only on Win32 (KERNEL32/USER32/GDI32) — no C runtime, no external
  DLLs, so it loads cleanly under any Wine/Proton version.

## Installation

1. Build `ddraw.dll` (see below) or take it from a release.
2. Copy `ddraw.dll` into the game's **`Enhanced/`** folder, next to
   `prophecy.exe`. Do **not** put it anywhere else.
3. **Mandatory:** tell Wine to prefer the native DLL. How depends on your
   launcher:

   **Steam:** *Properties → Launch Options*:

   ```
   WINEDLLOVERRIDES="ddraw=n" %command%
   ```

   **Heroic Games Launcher (GOG):** select the game → *Settings* (gear icon)
   → *Advanced* → **Environment Variables** → *Add* and enter:

   | Name               | Value     |
   | ------------------ | --------- |
   | `WINEDLLOVERRIDES` | `ddraw=n` |

   (No `%command%` suffix — that is Steam-only syntax. In Heroic,
   `WINEDLLOVERRIDES` goes into the environment variables table.)

   **Lutris:** game → *Configure* → *System options* → *Environment variables*
   → add `WINEDLLOVERRIDES` = `ddraw=n`.

   Without this override Wine loads its **builtin** ddraw and silently ignores
   the native DLL sitting next to the executable — no error, no log, and it
   looks like the shim is broken.

4. Play a cutscene. Then check `ddraw_shim.log` next to `prophecy.exe`:

   - Healthy: `DirectDrawCreate`, `CreateSurface ... primary=1`,
     `video window: using hwnd=...`, repeated `present: ... result=<height>`.
   - **No log file at all** → the DLL override did not take (step 3).
   - `unsupported <GUID> -> E_NOINTERFACE` lines → the renderer asked for a
     newer DirectDraw interface; the log tells you exactly which one.

## Building from source

You need a 32-bit MinGW-w64 cross-compiler (`i686-w64-mingw32-gcc`).
**64-bit will not work** — the game is 32-bit.

> **Gotcha:** the toolchain must live at a path **without spaces**. MinGW's
> `ld` splits its own library search paths on spaces and fails with
> `cannot find D:/.../default-manifest.o`. This is why `build.bat` expects the
> toolchain at `D:\stuff\mingw32` rather than inside this repo.

### Windows

1. Download a winlibs i686 build, e.g.
   `winlibs-i686-posix-dwarf-gcc-*-mingw-w64ucrt-*.zip` from
   <https://github.com/brechtsanders/winlibs_mingw/releases>, and extract it so
   that `i686-w64-mingw32-gcc.exe` is at `D:\stuff\mingw32\bin\` (or adjust
   `build.bat`, or have `i686-w64-mingw32-gcc` on `PATH`).
   MSYS2's `mingw-w64-i686-toolchain` package works too.
2. Run:

   ```
   build.bat
   ```

   which is equivalent to:

   ```
   i686-w64-mingw32-gcc -shared -O2 -fno-builtin -fno-strict-aliasing -Wall -Wextra ^
       -o ddraw.dll ddraw_shim.c ddraw.def ^
       -luser32 -lgdi32 -static-libgcc
   ```

### Linux

```
sudo apt install gcc-mingw-w64-i686        # Debian/Ubuntu; or your distro's mingw-w64
i686-w64-mingw32-gcc -shared -O2 -fno-builtin -fno-strict-aliasing -Wall -Wextra \
    -o ddraw.dll ddraw_shim.c ddraw.def \
    -luser32 -lgdi32 -static-libgcc
```

### Build notes

- `ddraw.def` is **required**: it exports the stdcall-decorated symbols under
  the plain undecorated names (`DirectDrawCreate = XDirectDrawCreate@12`, ...)
  that `GetProcAddress` resolves, mirroring the export set of Wine's builtin
  `ddraw.dll`. In practice `dshow.dll` only resolves `DirectDrawCreate`, but
  the rest are exported so nothing else trips.
- Every vtable stub has the exact `__stdcall` arity of the method it stands in
  for (checked against `ddraw.h`); a wrong arity would corrupt the stack on
  return. The build is warning-clean with `-Wall -Wextra` — treat any
  "incompatible pointer type" warning as a bug.
- The interface GUIDs are defined in the source, so no `dxguid`/`uuid`
  library is needed for the DLL itself.

## Testing

`test_shim.c` is a native Windows smoke test that loads the built `ddraw.dll`
and runs the renderer's exact call sequence — plus the `Blt`/`BltFast`/`Flip`/
`GetDC` fallback paths — against a window layout mimicking the game (unowned
main window + owned visible popup). If any vtable stub had the wrong arity,
the process would crash before finishing.

```
i686-w64-mingw32-gcc -O2 -o test_shim.exe test_shim.c -luser32 -lgdi32 -ldxguid -luuid
test_shim.exe path\to\ddraw.dll
```

Expected result: `ALL TESTS PASSED` (39 checks), and the produced
`ddraw_shim.log` shows the shim picking the owned popup window and presenting
every frame (`result=<height>` = all scanlines written).

## Files

| File           | Purpose                                             |
| -------------- | --------------------------------------------------- |
| `ddraw_shim.c` | The shim (single translation unit, Win32-only)      |
| `ddraw.def`    | Export definitions (undecorated stdcall aliases)    |
| `build.bat`    | Windows build script (DLL + smoke test)             |
| `test_shim.c`  | Native smoke test for the shim                      |

Build outputs (`ddraw.dll`, `test_shim.exe`) and the runtime log
(`ddraw_shim.log`) are git-ignored.
