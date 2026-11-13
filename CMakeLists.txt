cmake_minimum_required(VERSION 3.22)
project(JNGSFrameGen VERSION 3.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT ANDROID)
    message(STATUS
        "Configuring JNGS Frame Gen in host verification mode. "
        "For Android arm64-v8a (.levipack), configure with the Android NDK toolchain.")
endif()

add_library(framegen SHARED
    src/FrameGenConfig.cpp
    src/CameraSmoother.cpp
    src/FramePacer.cpp
    src/GlFrameInterpolator.cpp
    src/GlRenderDiagnostics.cpp
    src/HudRenderer.cpp
    src/FrameGenMod.cpp
    src/ConfigManager.cpp
)

target_include_directories(framegen PRIVATE
    src
    preloader_headers
    include
)

# Build-time stub of libpreloader.so so libjngsframegen.so records DT_NEEDED
# libpreloader.so and exact NDK/libc++ mangled undefined pl::* symbols.
# Do NOT ship libpreloader.so inside the .levipack; LeviLaunchroid injects
# its own runtime libpreloader.so into the game process.
add_library(preloader SHARED
    preloader_stub/preloader_stub.cpp
)
target_include_directories(preloader PRIVATE preloader_headers)
set_target_properties(preloader PROPERTIES
    OUTPUT_NAME "preloader"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/stub"
)

if(ANDROID)
    target_link_libraries(framegen PRIVATE
        EGL
        GLESv2
        GLESv3
        dl
        log
        preloader
    )
else()
    target_include_directories(preloader PRIVATE tests/host_stubs)
    target_include_directories(framegen PRIVATE tests/host_stubs)
    target_link_libraries(framegen PRIVATE
        dl
        preloader
    )

    enable_testing()
    add_executable(framegen_test_suite
        src/FrameGenConfig.cpp
        src/CameraSmoother.cpp
        src/FramePacer.cpp
        src/GlFrameInterpolator.cpp
    src/GlRenderDiagnostics.cpp
        src/HudRenderer.cpp
        src/FrameGenMod.cpp
        src/ConfigManager.cpp
        tests/framegen_test_suite.cpp
        tests/host_stubs/MemoryPatchStub.cpp
    )
    target_include_directories(framegen_test_suite PRIVATE
        src
        preloader_headers
        include
        tests/host_stubs
    )
    target_link_libraries(framegen_test_suite PRIVATE
        dl
        pthread
        fmt
    )
    target_compile_definitions(framegen_test_suite PRIVATE JNGS_HOST_TEST=1)
    target_compile_options(framegen_test_suite PRIVATE
        -Wall
        -Wextra
        -Werror -Wno-unused-private-field
        # Keep assert() live even in Release builds (NDEBUG would compile the
        # assertions out and turn every test-local variable into an
        # unused-variable error under -Werror).
        -UNDEBUG
        -O2
    )
    add_test(NAME FrameGenTestSuite COMMAND framegen_test_suite)
endif()

target_compile_options(framegen PRIVATE
    -Wall
    -Wextra
    -O2
    -ffunction-sections
    -fdata-sections
)

set_target_properties(framegen PROPERTIES
    LINK_FLAGS "-Wl,--gc-sections"
    OUTPUT_NAME "jngsframegen"
)
