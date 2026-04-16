#!/bin/bash
# Patch RyuGraph for MinGW compatibility
# Usage: ./scripts/patch_ryugraph.sh <kuzu-src-dir>
set -e

KZ="${1:-build/_deps/kuzu-src}"
cd "$KZ"

# 1. helpers.h — add cstdint at line 1
sed -i '1i #include <cstdint>' src/include/c_api/helpers.h
echo "[1/5] helpers.h: added cstdint"

# 2. helpers.cpp — add cstdint at line 1
sed -i '1i #include <cstdint>' src/c_api/helpers.cpp
echo "[2/5] helpers.cpp: added cstdint"

# 3. local_file_system.cpp — add sys/stat.h at line 1
sed -i '1i #include <sys/stat.h>' src/common/file_system/local_file_system.cpp
echo "[3/5] local_file_system.cpp: added sys/stat.h"

# 4. buffer_manager.cpp — replace #if defined(_WIN32) (include guard for eh.h)
#    with #if defined(_WIN32) && defined(_MSC_VER) so MinGW skips SEH code
BM=src/storage/buffer_manager/buffer_manager.cpp
# Only replace the FIRST occurrence which is the eh.h include guard at line ~22
sed -i '0,/^#if defined(_WIN32)$/s//\n#if defined(_WIN32) \&\& defined(_MSC_VER)\n/' "$BM"
# Remove the blank line we accidentally created before it
sed -i '/^$/N;/^\n#if defined(_WIN32) \&\& defined(_MSC_VER)/d' "$BM" || true
# Re-add the line properly
sed -i '0,/^#if defined(_WIN32) \&\& defined(_MSC_VER)/{s/^#if defined(_WIN32) \&\& defined(_MSC_VER)/#if defined(_WIN32) \&\& defined(_MSC_VER)/}' "$BM" || true
echo "[4/5] buffer_manager.cpp: guarded SEH with _MSC_VER"

# 5. glob.hpp — use getenv instead of _dupenv_s
sed -i 's/_dupenv_s(\&result, \&length, name)/(result = getenv(name), length = result ? strlen(result) : 0, (void)0)/' third_party/glob/glob/glob.hpp
echo "[5/5] glob.hpp: replaced _dupenv_s with getenv"

# 6. CMakeLists.txt — add ws2_32 for WIN32
CL=src/CMakeLists.txt
# The WIN32 block has: set(RYU_LIBRARIES ${RYU_LIBRARIES} Threads::Threads)
# Add ws2_32 to it
if grep -q "Threads::Threads" "$CL"; then
    sed -i 's/set(RYU_LIBRARIES ${RYU_LIBRARIES} Threads::Threads)/set(RYU_LIBRARIES ${RYU_LIBRARIES} Threads::Threads ws2_32)/' "$CL"
else
    # Fallback: add after the if(WIN32) line
    sed -i '/if(WIN32)/a\    set(RYU_LIBRARIES ${RYU_LIBRARIES} ws2_32)' "$CL"
fi
echo "[6/6] CMakeLists.txt: added ws2_32"

echo "All patches applied successfully"
