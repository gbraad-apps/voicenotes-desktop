#!/bin/bash
# Package Voicenotes for Windows distribution

set -e

VERSION="0.1.0"
BUILD_DIR="build-windows"
PACKAGE_NAME="voicenotes-${VERSION}-windows-x64"
DIST_DIR="$PACKAGE_NAME"
MINGW_SYSROOT="/usr/x86_64-w64-mingw32/sys-root/mingw"

echo "=========================================="
echo "Packaging Voicenotes for Windows"
echo "=========================================="
echo ""

if [ ! -f "$BUILD_DIR/voicenotes.exe" ]; then
    echo "Error: voicenotes.exe not found!"
    echo "Run ./build-windows.sh first"
    exit 1
fi

echo "Creating distribution directory: $DIST_DIR"
rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR"

echo "Copying executable..."
cp "$BUILD_DIR/voicenotes.exe" "$DIST_DIR/"

echo ""
echo "Analyzing DLL dependencies..."
if command -v x86_64-w64-mingw32-objdump &> /dev/null; then
    echo "Required DLLs:"
    x86_64-w64-mingw32-objdump -p "$BUILD_DIR/voicenotes.exe" | grep "DLL Name:" | sort -u
    echo ""
fi

echo "Copying required DLLs..."

copy_dll() {
    local name="$1"
    local pkg="$2"
    if [ -f "$MINGW_SYSROOT/bin/$name" ]; then
        cp "$MINGW_SYSROOT/bin/$name" "$DIST_DIR/"
        echo "  ✓ $name"
    else
        echo "  ✗ $name NOT FOUND"
        [ -n "$pkg" ] && echo "    Install: sudo dnf install $pkg"
        exit 1
    fi
}

copy_dll_optional() {
    local name="$1"
    if x86_64-w64-mingw32-objdump -p "$BUILD_DIR/voicenotes.exe" 2>/dev/null | grep -qi "$name"; then
        if [ -f "$MINGW_SYSROOT/bin/$name" ]; then
            cp "$MINGW_SYSROOT/bin/$name" "$DIST_DIR/"
            echo "  ✓ $name"
        fi
    fi
}

copy_dll "SDL3.dll"              "mingw64-SDL3"
copy_dll "libcurl-4.dll"         "mingw64-curl"
copy_dll "libssl-3-x64.dll"      "mingw64-openssl"
copy_dll "libcrypto-3-x64.dll"   "mingw64-openssl"
copy_dll_optional "libwinpthread-1.dll"
copy_dll_optional "libstdc++-6.dll"
copy_dll_optional "libgcc_s_seh-1.dll"
copy_dll_optional "libzstd.dll"
copy_dll_optional "libbrotlidec.dll"
copy_dll_optional "libbrotlicommon.dll"
copy_dll_optional "zlib1.dll"

echo ""
echo "Listing package contents:"
ls -lh "$DIST_DIR/"

echo ""
echo "=========================================="
echo "Package created successfully!"
echo "=========================================="
echo "Directory: $DIST_DIR/"
echo ""
echo "Place a Whisper model next to voicenotes.exe:"
echo "  ggml-base.bin  (~141 MB, recommended)"
echo "  ggml-tiny.bin  (~39 MB,  faster)"
echo ""
echo "Test with Wine:"
echo "  cd $DIST_DIR && wine voicenotes.exe"
echo ""

if command -v zip &> /dev/null; then
    echo "Creating ZIP archive..."
    zip -r "${PACKAGE_NAME}.zip" "$DIST_DIR"
    echo "ZIP: ${PACKAGE_NAME}.zip  ($(du -sh ${PACKAGE_NAME}.zip | cut -f1))"
fi
