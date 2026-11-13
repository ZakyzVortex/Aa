# Cross-compile toolchain for Android arm64-v8a (API 26) using the NATIVE
# aarch64 clang against the Android NDK sysroot.
#
# Why "Generic" system name: CMake's built-in Android module insists on using
# the NDK's linux-x86_64 prebuilt clang, which cannot execute on this aarch64
# host. We instead drive clang directly as a cross-compiler and define the
# ANDROID variable ourselves so the project's `if(ANDROID)` link branch runs.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_SYSTEM_VERSION 26)
set(ANDROID TRUE CACHE BOOL "Android build" FORCE)
set(CMAKE_ANDROID_ARCH_ABI arm64-v8a)

# NDK root: prefer $ANDROID_NDK_HOME, else the sandbox default.
if(DEFINED ENV{ANDROID_NDK_HOME} AND NOT "$ENV{ANDROID_NDK_HOME}" STREQUAL "")
    set(NDK_ROOT "$ENV{ANDROID_NDK_HOME}")
else()
    set(NDK_ROOT "/workspace/ndk/android-ndk-r26d")
endif()
set(NDK_PREBUILT "${NDK_ROOT}/toolchains/llvm/prebuilt/linux-x86_64")
set(NDK_SYSROOT "${NDK_PREBUILT}/sysroot")
set(CMAKE_SYSROOT "${NDK_SYSROOT}")

# The NDK's clang-17 headers (incl. arm_neon.h) are incompatible with the
# clang-19 driver, so we compile with the host clang's own resource headers.
# For linking we add the NDK's aarch64 Android runtime libs (builtins +
# libunwind) via -L, and an empty libssp_nonshared.a (the clang-19 driver
# emits -lssp_nonshared for this target though Android has no such library).
set(NDK_CLANG_LIB "${NDK_PREBUILT}/lib/clang/17/lib/linux/aarch64")

# Native aarch64 compilers.
set(CMAKE_C_COMPILER /usr/bin/clang)
set(CMAKE_CXX_COMPILER /usr/bin/clang++)

# Force the Android target on every compile/link invocation.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} --target=aarch64-linux-android26")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} --target=aarch64-linux-android26")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --target=aarch64-linux-android26 -L${NDK_CLANG_LIB} -L${NDK_ROOT}/empty")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} --target=aarch64-linux-android26 -L${NDK_CLANG_LIB} -L${NDK_ROOT}/empty")

set(CMAKE_FIND_ROOT_PATH "${NDK_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
