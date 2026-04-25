#!/usr/bin/env bash
# Downloads whisper.cpp source. Run once before building.
set -euo pipefail

WHISPER_TAG="v1.7.4"
TARGET="whisper.cpp"

echo "==> Setting up voicenotes"

if [ -d "$TARGET/.git" ]; then
    echo "==> whisper.cpp already present at $WHISPER_TAG"
else
    echo "==> Cloning whisper.cpp $WHISPER_TAG..."
    git clone --branch "$WHISPER_TAG" --depth 1 --recurse-submodules \
        https://github.com/ggerganov/whisper.cpp.git "$TARGET"
fi

echo ""
echo "==> Done. Build with: ./build-linux.sh"
echo ""
echo "==> Download a model (if you don't have one):"
echo "    curl -L -o ggml-base.bin https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.bin"
