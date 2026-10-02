#!/usr/bin/env bash
# Builds and runs the encoder host tests (Linux, g++).
#
# libjpeg-turbo is built from source because the console uses its 3.x API
# (tj3*), which distribution packages older than 3.0 do not have. SIMD is off,
# as it is in the Wii U build.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
src="$here/../../src"
build="${BUILD_DIR:-$here/build}"
ljt_version="${LJT_VERSION:-3.1.0}"
mkdir -p "$build"

if [ ! -f "$build/ljt/lib/libturbojpeg.a" ] && [ ! -f "$build/ljt/lib64/libturbojpeg.a" ]; then
    echo "--- building libjpeg-turbo $ljt_version"
    rm -rf "$build/ljt-src" "$build/ljt-build"
    git clone -q --depth 1 --branch "$ljt_version" https://github.com/libjpeg-turbo/libjpeg-turbo.git "$build/ljt-src"
    cmake -S "$build/ljt-src" -B "$build/ljt-build" -DCMAKE_BUILD_TYPE=Release -DENABLE_SHARED=OFF \
        -DWITH_SIMD=OFF -DCMAKE_INSTALL_PREFIX="$build/ljt" > "$build/ljt-configure.log"
    cmake --build "$build/ljt-build" --target install -j "$(nproc)" > "$build/ljt-build.log"
fi
ljt_lib="$build/ljt/lib/libturbojpeg.a"
[ -f "$ljt_lib" ] || ljt_lib="$build/ljt/lib64/libturbojpeg.a"

sources=(
    "$src/ImageEncoder.cpp"
    "$src/YuvConvert.cpp"
    "$src/JpegStitch.cpp"
    "$src/retain_vars.cpp"
    "$here/stubs/stubs.cpp"
    "$here/fakes.cpp"
    "$here/encoder_test.cpp"
)
flags=(-std=c++20 -Wall -I"$here/stubs" -I"$src" -I"$build/ljt/include" -pthread)

# Release configuration: compile only, to see the warnings the console's release
# build would hit (log calls compile away there, leaving variables unused).
echo "--- compiling (release configuration, warnings only)"
g++ "${flags[@]}" -O2 -fsyntax-only "$src/ImageEncoder.cpp" "$src/YuvConvert.cpp" "$src/JpegStitch.cpp"

echo "--- compiling (debug, AddressSanitizer + UndefinedBehaviorSanitizer)"
g++ "${flags[@]}" -DDEBUG -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined \
    -fno-sanitize-recover=undefined "${sources[@]}" "$ljt_lib" -o "$build/encoder_test"

echo "--- running"
ASAN_OPTIONS="detect_leaks=1:abort_on_error=1" "$build/encoder_test"
