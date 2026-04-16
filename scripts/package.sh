#!/bin/bash
# package.sh — Build + package Lulu for Windows
# Usage: ./scripts/package.sh [version]
# Outputs: dist/lulu-{version}-win64.zip

set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
export PATH="/c/msys64/mingw64/bin:$PATH"

# Determine version from version.h
VERSION="${1:-}"
if [ -z "$VERSION" ]; then
    VERSION=$(grep 'LULU_VERSION_STR' runtime/src/include/version.h | head -1 | sed 's/.*"\([^"]*\)".*/\1/')
fi

echo "========================================="
echo " Packaging Lulu v${VERSION}"
echo "========================================="

DIST="dist/lulu-${VERSION}-win64"
DIST_ZIP="dist/lulu-${VERSION}-win64.zip"

# Clean previous
rm -rf "dist/lulu-${VERSION}-win64" "$DIST_ZIP"
mkdir -p "$DIST/tools" "$DIST/state" "$DIST/workspace" "$DIST/tg_data"

# ---- Build agent + updater via CMake ----
echo "[1/3] Building agent + updater..."
mkdir -p build && cd build
cmake -G "MinGW Makefiles" ..
cmake --build . -- -j$(nproc 2>/dev/null || echo 4)

# Copy RyuGraph DLL
cp _deps/kuzu-build/src/libryu_shared.dll . 2>/dev/null || true
cd "$ROOT"

# ---- Build tool DLLs ----
echo "[2/3] Building tool DLLs..."
mkdir -p tools
for f in runtime/tools/*.c; do
    name=$(basename "$f" .c)
    echo "  $name.dll"
    gcc -shared -std=c11 -D_CRT_SECURE_NO_WARNINGS -DTOOL_BUILDING_DLL \
        -I runtime/src/include \
        "$f" runtime/src/cJSON.c runtime/src/sandbox.c \
        -o "tools/$name.dll" -lSDL3 -lSDL3_image 2>/dev/null || \
    gcc -shared -std=c11 -D_CRT_SECURE_NO_WARNINGS -DTOOL_BUILDING_DLL \
        -I runtime/src/include \
        "$f" runtime/src/cJSON.c runtime/src/sandbox.c \
        -o "tools/$name.dll" 2>/dev/null || \
    echo "  SKIP $name (missing deps)"
done

# ---- Package ----
echo "[3/3] Packaging..."
cp build/agent.exe build/libryu_shared.dll build/updater.exe "$DIST/"
cp agent.json "$DIST/"
cp tools/*.dll "$DIST/tools/" 2>/dev/null || true

# Config template
cat > "$DIST/config.json" << 'CFGEOF'
{"model":"glm-5.1","endpoint":"https://api.z.ai/api/coding/paas/v4/chat/completions","apikey":"","max_tokens":4096,"temperature":0.7}
CFGEOF

# Install marker
echo "{\"version\":\"v${VERSION}\"}" > "$DIST/install.json"

# Zip
cd dist
zip -r "$DIST_ZIP" "lulu-${VERSION}-win64/"
cd "$ROOT"

echo ""
echo "========================================="
echo " Package: $DIST_ZIP"
echo " Size:    $(du -sh "$DIST_ZIP" | cut -f1)"
echo "========================================="

# ---- Optional: NSIS installer ----
if command -v makensis &>/dev/null; then
    echo "[NSIS] Building installer..."
    makensis -DPRODUCT_VERSION="$VERSION" installer/lulu.nsi
    echo " Installer: dist/lulu-${VERSION}-setup.exe"
fi

echo "Done."
