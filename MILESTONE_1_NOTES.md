# JNGS Frame Gen 3.2.0 — Milestone 1 notes

## Changes made

1. Removed the private Levi FPS-mod counter integration, including ARM64 instruction decoding, runtime library scanning, and direct counter writes. The mod no longer modifies another mod's counters.
2. `Single-Swap` is now accounted as one submitted image. It can still apply the temporal screen-space visual blend, but it does not claim 2x/3x/4x additional presentation.
3. `Multi-Swap` remains the only path that attempts additional EGL submissions. Its telemetry counts successful extra swap calls, not guaranteed display scanouts.
4. Removed the shader condition that forced interpolation weight to zero whenever Motion Blur was disabled. Interpolation blend strength is now independent of that toggle.
5. Updated the ModMenu descriptions and README so the modes are not misrepresented.
6. Removed artificial binary/package size padding and random marker data. Package size now reflects actual code/assets.
7. Bumped project metadata to 3.2.0.

## Verification

- Host test target builds successfully.
- `ctest --test-dir build-debug --output-on-failure`: 1/1 test passed.
- The tests use mocked EGL/GLES functions. They validate call counts, timestamp ordering, state restoration, and accounting logic; they do not prove that SurfaceFlinger displays every submitted buffer.
- The Android ARM64 build was not produced in this environment because an Android NDK toolchain is not installed here.

## Important limitations still open

- The current motion model is global camera motion, not per-pixel optical flow.
- Single-Swap is not additional-frame generation.
- Multi-Swap submits additional buffers, but the Android compositor/display may queue, delay, or drop them. A successful `eglSwapBuffers` is not proof of scanout.
- `eglPresentationTimeANDROID` availability and behavior must be measured on the target device.
- Frame capture currently uses `glCopyTexSubImage2D` from the default framebuffer; framebuffer preservation and behavior on the Honor 600 Lite must be validated at runtime.
- Depth and per-object motion-vector access have not been established from the supplied Minecraft binary.

## Next milestone

Add on-device instrumentation for swap-call timestamps, requested presentation timestamps, GL errors, surface dimensions, and measured frame pacing. Then test Multi-Swap at 2x before attempting optical flow or depth-aware interpolation.
8. Added a low-strength optional directional blur pass driven by the existing global camera-motion estimate. The Motion Blur toggle now controls this pass; it no longer gates or changes the base interpolation blend.
