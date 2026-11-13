#!/usr/bin/env sh
# Build the Android arm64-v8a libjngsframegen.so and package FrameGen.levipack.
#
# Prereqs (host): clang, cmake, ninja, lld, python3, and an Android NDK.
#   export ANDROID_NDK_HOME=/path/to/android-ndk-r26d
# If ANDROID_NDK_HOME is unset, defaults to /workspace/ndk/android-ndk-r26d.
#
# This script works on aarch64 Linux hosts (e.g. Termux) where the NDK's
# linux-x86_64 prebuilt clang cannot run; it uses the native clang as a
# cross-compiler against the NDK sysroot (see android.toolchain.cmake).

set -e

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
NDK_ROOT=${ANDROID_NDK_HOME:-/workspace/ndk/android-ndk-r26d}
NDK_PREBUILT="$NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64"
NDK_CLANG_LIB="$NDK_PREBUILT/lib/clang/17/lib/linux/aarch64"
RESOURCE_LIB=/usr/lib/llvm19/lib/clang/19/lib/aarch64-unknown-linux-android26

echo "==> NDK root: $NDK_ROOT"
[ -d "$NDK_PREBUILT/sysroot" ] || { echo "error: NDK sysroot not found"; exit 1; }

# --- One-time host setup -----------------------------------------------
# 1) Empty libssp_nonshared.a: clang-19's driver emits -lssp_nonshared for
#    this target, but Android has no such library.
mkdir -p "$NDK_ROOT/empty"
if [ ! -f "$NDK_ROOT/empty/libssp_nonshared.a" ]; then
    (cd "$NDK_ROOT/empty" && ar rcs libssp_nonshared.a)
fi

# 2) Symlink the NDK's aarch64 Android builtins into the host clang's
#    resource dir so clang finds libclang_rt.builtins.a for the android target.
if [ -f "$NDK_CLANG_LIB/libclang_rt.builtins-aarch64-android.a" ]; then
    mkdir -p "$RESOURCE_LIB"
    ln -sf "$NDK_CLANG_LIB/libclang_rt.builtins-aarch64-android.a" \
        "$RESOURCE_LIB/libclang_rt.builtins.a"
fi

# --- Configure + build --------------------------------------------------
cmake -S "$ROOT" -B "$ROOT/build-android" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/android.toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$ROOT/build-android"

# --- Package + verify ---------------------------------------------------
python3 "$SCRIPT_DIR/package_levipack.py" \
    --library "$ROOT/build-android/libjngsframegen.so" \
    --icon "$ROOT/assets/icon.png" \
    --output "$ROOT/build-android/FrameGenV3.3.1.levipack"
python3 "$SCRIPT_DIR/verify_levipack.py" \
    "$ROOT/build-android/FrameGenV3.3.1.levipack"

echo "==> Done: $ROOT/build-android/FrameGenV3.3.1.levipack"
