#!/bin/bash
set -e

echo "Cross-compiling voicenotes for Windows (MinGW)..."

for pkg in mingw64-gcc mingw64-gcc-c++ mingw64-SDL3; do
    if ! rpm -q $pkg &>/dev/null; then
        echo "Missing: $pkg  (sudo dnf install $pkg)"
        exit 1
    fi
done

mkdir -p build-windows && cd build-windows
cmake .. \
    -DCMAKE_TOOLCHAIN_FILE=../toolchain-mingw64.cmake \
    -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)

echo ""
echo "Done: build-windows/voicenotes.exe"
