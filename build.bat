@echo off
rem Build the 32-bit ddraw.dll shim and its native smoke test.
setlocal
rem NOTE: the toolchain must live at a path WITHOUT spaces (ld splits its
rem own library paths otherwise), hence D:\stuff\mingw32, not this folder.
set "GCC=D:\stuff\mingw32\bin\i686-w64-mingw32-gcc.exe"
if not exist "%GCC%" set "GCC=i686-w64-mingw32-gcc.exe"

"%GCC%" -shared -O2 -fno-builtin -fno-strict-aliasing -Wall -Wextra ^
    -o ddraw.dll ddraw_shim.c ddraw.def ^
    -luser32 -lgdi32 -static-libgcc || exit /b 1

"%GCC%" -O2 -Wall -Wextra -o test_shim.exe test_shim.c -luser32 -lgdi32 -ldxguid -luuid || exit /b 1

echo Build OK: ddraw.dll + test_shim.exe
