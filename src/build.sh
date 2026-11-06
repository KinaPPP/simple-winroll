#!/bin/sh
# Build on Linux with MinGW-w64 (x86_64-w64-mingw32-g++)
set -e
x86_64-w64-mingw32-windres -O coff app.rc -o app.res
x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -municode -mwindows -static -static-libgcc -static-libstdc++ \
  main.cpp app.res -o simple-winroll.exe \
  -luser32 -lgdi32 -lshell32 -ldwmapi -lgdiplus -lversion -ladvapi32 -lole32 -luuid -lpropsys -lshlwapi
x86_64-w64-mingw32-strip simple-winroll.exe
