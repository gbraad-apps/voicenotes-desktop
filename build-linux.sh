#!/bin/bash
set -e

echo "Building voicenotes for Linux..."

for dep in sdl3 libcurl; do
    if ! pkg-config --exists $dep 2>/dev/null; then
        echo "Missing: $dep"
        echo "  Fedora: sudo dnf install SDL3-devel libcurl-devel"
        echo "  Ubuntu: sudo apt install libsdl3-dev libcurl4-openssl-dev"
        exit 1
    fi
done

mkdir -p build-linux && cd build-linux
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)

echo ""
echo "Done: build-linux/voicenotes"
echo "Run:  ./build-linux/voicenotes"
