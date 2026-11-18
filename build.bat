@echo off
rem Build with MSYS2 (https://www.msys2.org): run this from the "MSYS2 UCRT64" shell's gcc on PATH,
rem after: pacman -S mingw-w64-ucrt-x86_64-gcc
if not exist out mkdir out
windres src/app.rc -O coff -o out/app.res.o || exit /b 1
gcc -O2 -std=c11 -municode -mwindows -static -s -o "out/Input Overlay.exe" src/main.c src/render.c src/demo.c src/apm.c out/app.res.o -luser32 -lgdi32 -lshell32 -lcomctl32 -lcomdlg32 -ldwmapi -lwinmm -ld3d11 -ldxgi -ldxguid || exit /b 1
echo Built out\Input Overlay.exe
