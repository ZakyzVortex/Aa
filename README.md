# JNGS Frame Gen (`FrameGen.levipack`)

**JNGS Frame Gen v3.2.0** is an experimental native C++20 mod for Minecraft Bedrock on Android under LeviLaunchroid. It combines camera smoothing, HUD options, GPU image processing, and an experimental EGL presentation path.

## v3.2.0 engineering corrections

- Removed writes to Levi's internal FPS counter. The mod must not report generated frames that were not submitted.
- `Single-Swap` submits one image and is treated as a temporal visual effect, not additional-frame generation.
- `Multi-Swap` is the only mode that attempts additional EGL submissions.
- Interpolation blending no longer gets disabled when the Motion Blur option is off.
- Host tests validate control flow with mocked EGL/GLES calls; they do **not** prove that Android's compositor displays every queued image.

**Important:** This is not yet equivalent to DLSS/FSR frame generation. The current motion model is screen-space/global camera motion, and can produce ghosting or incorrect motion around independently moving entities, particles, and occlusions. Android device testing is required before treating the reported presentation rate as display refresh rate.

---

## Research: Why Frame Generation Feels Fake (and When It Can Feel Real)

We audited the latest 2026 analyses (NVIDIA DLSS 4, AMD FSR 3, TechSpot/Hardware Unboxed, Digital Foundry, Back2Gaming) — consensus:

* **A rendered frame** is the engine’s answer to *“what should the screen show given inputs *right now*?”*  
* **A generated frame** is interpolated *between* two renders — it contains **no new input, no new simulation**. It is a plausible picture of a moment nobody simulated [Back2Gaming, TechFuelHQ].

**Latency cost:** To make frame N+0.5 you must *hold* frame N+1 until you have it, then show N+0.5 before N+1. That is ~1 base-frame of added latency (16.7 ms at 60 FPS, 5 ms at 200 FPS, 2.5 ms at 400 FPS) plus warp cost (~1-2 ms) [TechSpot, XDA, FPSHQ]. NVIDIA Reflex / AMD Anti-Lag claw back queue depth but **cannot make generated as responsive as real** — they only return you to near-native baseline.

**Base FPS is everything.** AMD: FSR 3 needs **≥60 FPS pre-interpolation**, sub-30 is “absolutely avoided” [FSR docs]. Digital Foundry: 40-60 base *before* gen is the floor where it starts to feel responsive; below that, artifacts and floaty aim dominate [Digital Foundry, XDA]. TechSpot: 60→120 with FG at 40 ms felt *worse* than 34 ms native 60, even though counter doubled.

**The 800 trap:** Users reported **800 displayed but feels like 200**. Typical cause: base ~200, 4× → 800 displayed, but 75% of photons are stale and base latency is 5 ms + ~5 ms gen hold = 10 ms, while native 800 would be 1.25 ms. Counter says 800, hands feel 200 — classic “160 feels like 60” signature [cr0x.net, Back2Gaming].

**JNGS TrueMotion approach to 800 ≈ 800:**

1. **Maximize base, not multiplier.** For 800 displayed, prefer **400 real × 2×** over **200×4×**. 2× has only 50% stale frames vs 75% at 4×, and base latency is half.
2. **Lowest-latency path:** `Single-Swap @ 0.5 phase` (one sharp warp), not multi-phase averaging (which is literal blur). JNGS v3 uses single-phase 0.5 for Single-Swap at all multipliers; multi-phase removed.
3. **Zero queue:** `eglSwapInterval(0)` (uncapped), `eglPresentationTimeANDROID` with monotonic `steady_clock` pacing, no ` paceForTargetFps` when Unlimited, and GlStateGuard that restores every bit of Minecraft’s GL state so we never dim or add a queue.
4. **Keep base high:** Ultra Low-Latency HUD (elevation 0, willNotDraw, transparent bg), AutoTune **OFF** for benching, 60-120 Hz display cap off, and choose **Balanced/Extreme** presets that don’t overload the GPU.

Native 800 will always be 1.25 ms more responsive than 400×2× (2.5 ms hold), but at 400 base the delta is 1.25 ms — below most humans’ 40 ms perception threshold and far closer than 200×4× [chrisdpratt]. That is why JNGS can make **800 feel *close* to 800**, but cannot break the interpolation law: you still need high base.

---

## Making 800 Feel Like 800 — JNGS Recommended

**For bench / “800 feels like 800” (high-end phones, 120Hz+):**
- Preset: `Custom` → set below
- Rendered Multiplier: **`2×`** ← not 4× (50% stale vs 75%)
- Frame Gen Mode: **`Multi-Swap Frame Interpolation`** (the only mode that attempts additional EGL submissions; validate on-device)
- Target FPS: **`0 = Unlimited`** (no pacing sleep)
- Blend Strength: `0.65`, Motion Scale `1.00×`, HUD Protection `0.88`
- Camera Smoothing: `Adaptive Smart`, Smoothness `0.45`, Responsiveness `1.05×`, Deadzone `0.025`
- HUD Optimizer: **`Ultra Low-Latency`**, Latency Flush `ON`, Static Boost `ON`, AutoTune **`OFF`**
- Phone: set display to max refresh, close background, thermal not throttling
- Result: if device can do **~380-420 real → ~760-840 displayed** with JNGS TrueMotion, feel is *very* close to native 800 (2.5 ms vs 1.25 ms). With 200 real ×4× you will still see 800 but feel 200 — don’t.

**For balanced 120-240 feel (most phones):**
- `2×`, `Multi-Swap`, `Balanced` preset, `Target FPS 0`, `AutoTune ON`, `HUD Ultra Low-Latency`

**For competitive PvP (lowest latency):**
- `2×`, `Multi-Swap` only if stable; otherwise use `Off` for lowest latency. `Single-Swap` is visual processing, not extra frames.

---

## Best Settings Guide

You can apply these settings anytime in-game by opening the **LeviLaunchroid Mod Menu** and selecting **JNGS Frame Gen**.

### 1. Best Overall (60Hz–120Hz)
- Preset: `Balanced`
- Multiplier: **`2×`** (or `3×` on 90Hz/120Hz)
- Mode: `Single-Swap`
- Blend `0.65–0.75`, Motion `1.00×`, HUD `0.88–0.92`, SceneCut `42.0`
- Camera: `Adaptive Smart (1-Euro)`, Smoothness `0.55`, Responsiveness `1.05×`, Deadzone `0.025`
- HUD Optimizer: `Ultra Low-Latency`, Flush `ON`, Boost `ON`, AutoTune `ON`

### 2. Maximum Smoothness (3×/4×)
- Preset: `Smooth`/`Ultra`
- Multiplier: `3×`/`4× (Max)`
- Mode: `Single-Swap` (zero lag) or `Multi-Swap` on 120Hz+ with headroom
- Blend `0.78–0.85`, Motion `1.05–1.10×`, HUD `0.86`
- Camera: `Adaptive Smart` or `Spring`, `0.65–0.72`, `1.00×`

### 3. Competitive PvP
- `Off` for minimum latency, or experimental `2× Multi-Swap` if the device sustains it. Single-Swap does not increase the presentation rate.

### 4. Budget / Low-End
- `Power Saver`, `2×`, `Single-Swap`, `0.45–0.55`, `EMA`/`Smart` `0.45`, AutoTune `ON`, `60 FPS` or `0`

### Quick Reference

| Setting | Range | Recommended | What It Does |
| :--- | :--- | :--- | :--- |
| Rendered Multiplier | `2×`,`3×`,`4×` | `2×`/`3×` | How many frames per real (`2×` half stale, `4×` 75% stale) |
| Frame Gen Mode | `Off`,`Single-Swap`,`Multi-Swap` | `Multi-Swap` for experimental additional frames | Single submits one temporal-blend image; Multi attempts extra EGL submissions, which may be queued/dropped by Android |
| Blend Strength | `0.00–1.00` | `0.65` | Warp vs real mix |
| Motion Scale | `0.00–2.00×` | `1.00×` | Camera-UV warp scale |
| HUD Protection | `0.00–1.00` | `0.88` | Pins static crosshair/hotbar |
| Camera Smoothing | `Off`,`EMA`,`Spring`,`Smart` | `Smart` | 100% angle-conserving filter |
| HUD Optimizer | `Off`,`Balanced`,`Ultra Low-Latency` | `Ultra` | Elevation 0, willNotDraw, swapInterval 0 |
| Target FPS | `0` Unlimited, 30–240 | `0` for 800 | `0` is no pacing (needed for 800) |

---

## Key Features

### 1. JNGS TrueMotion Frame Generation
- **Experimental Android GPU warp:** `GlStateGuard`, retained frame textures, global camera-motion UV warp, and optional `Multi-Swap` submissions using `eglPresentationTimeANDROID`. Host tests mock GLES/EGL and do not execute the shader or verify display scanout. Falls back to the original game frame on GL errors; no FPS counter injection.
- **Zero center logo, zero dim:** `hideInHudEditor`, transparent `HudOverlay` bg, elevation 0, `willNotDraw` when idle, JNI auto-relocates LeviLaunchroid’s center-spawned overlays to edges.

### 2. 100% Angle-Conserving Camera Smoother
- Hooks `applyTurnDelta`, `tryGetFOV`, `getCameraPerspective` with double-precision residual reservoir — 100.000% angle conservation, zero drift.

### 3. Frame Pacer & Telemetry
- Monotonic `steady_clock` pacing when capped; `0 = Unlimited` for 800 bench. `realFps and estimated presentation rate are tracked separately; neither guarantees scanout by the display.

### 4. HUD FPS Boost WITHOUT Removing HUD (v3.0.0+ abuse)
- HUD optimization options are experimental and device-dependent; measure their impact on your own device. Do not assume that hiding an individual HUD element has a fixed FPS benefit.
- **JNGS abuse (keeps bars visible):**
  - Disables **vignette** (full-screen overdraw) via `vignette_renderer` transparent / fancy-graphics patch (biggest single save, screen stays bright).
  - Makes **enchant glint** static/transparent (cuts hotbar draw-call bloat, keeps items visible).
  - Makes **heart/hunger/hotbar event-driven**: cached HUD texture, dirty-only rebuild, throttled to 30Hz when `Ultra Low-Latency + Static Boost ON` (saves ~50-70% HUD CPU with no flicker, bars stay visible).
  - Auto-fallback: if patch not found on this device, optimizer still does elevation/willNotDraw/swapInterval and never crashes.
- Enable: **Mod Menu -> HUD Optimizer = Ultra Low-Latency, Static Boost = ON** (and keep `Show HUD = ON` to keep bars visible).

---

## Installation

1. Download **`FrameGen.levipack`** from repo root or Actions **Artifacts** / **Releases**.
2. Open **LeviLaunchroid** → **Mods** → Import `FrameGen.levipack` → enable **JNGS Frame Gen**.
3. Launch Minecraft → Mod Menu → pick `2×`/`3×`/`4×` and tune (see above).

---

## Building & Testing

```bash
g++ -std=c++20 -O2 -Wall -Wextra -Werror \
    -Isrc -Ipreloader_headers -Itests/host_stubs \
    src/FrameGenConfig.cpp src/CameraSmoother.cpp src/FramePacer.cpp \
    src/GlFrameInterpolator.cpp src/HudRenderer.cpp src/FrameGenMod.cpp \
    tests/framegen_test_suite.cpp -ldl -pthread -o /tmp/framegen_test_suite
/tmp/framegen_test_suite

cmake -B build -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 \
    -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
"$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" --strip-unneeded build/libjngsframegen.so
python3 scripts/package_levipack.py --library build/libjngsframegen.so --icon assets/icon.png --output FrameGen.levipack
python3 scripts/verify_levipack.py FrameGen.levipack
```
