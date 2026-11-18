#!/bin/sh
# Cross-compile on Linux with zig (bundles mingw-w64 headers/libs). Produces a
# single static x64 exe with no runtime dependencies beyond Windows itself.
#   pip install ziglang==0.13.0   (or use any zig 0.13 / mingw-w64 toolchain)
set -e
cd "$(dirname "$0")"
ZIG="${ZIG:-python3 -m ziglang}"
mkdir -p out
rm -f out/*.pdb
$ZIG cc -target x86_64-windows-gnu -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter \
  -municode -Wl,--subsystem,windows -s -o out/input-overlay.exe \
  src/main.c src/render.c src/demo.c src/apm.c src/app.rc \
  -luser32 -lgdi32 -lshell32 -lcomctl32 -lcomdlg32 -ldwmapi -lwinmm -ld3d11 -ldxgi -ldxguid
# Alternative with mingw-w64 gcc:
#   x86_64-w64-mingw32-windres src/app.rc -O coff -o out/app.res.o
#   x86_64-w64-mingw32-gcc -O2 -municode -mwindows -static -o out/input-overlay.exe \
#     src/main.c src/render.c src/demo.c src/apm.c out/app.res.o -luser32 -lgdi32 -lshell32 -lcomctl32 -lcomdlg32 -ldwmapi -lwinmm -ld3d11 -ldxgi -ldxguid
rm -f out/*.pdb out/*.lib
