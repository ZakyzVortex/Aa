# 3.3.5 – fixes for misaligned / desynchronized generated frames

All changes are in `src/GlFrameInterpolator.cpp/.hpp` (+ version strings).

1. **Previous-output capture** – `mTexPrevSynthesis` was copied *after* `eglSwapBuffers`
   (back buffer undefined) and without scaling (only the bottom-left crop when
   `synthesisScale < 1`). Now captured from the presented image *before* the swap,
   scaled to the full frame (`captureOutputToPrevSynthesis`).
2. **Hook-time capture is state-safe** – the world capture inside the game's
   `glDisable(GL_DEPTH_TEST)` hook used to leave the framebuffer bound to 0 and ignore
   `GL_SCISSOR_TEST`. It now restores read/draw framebuffers and scissor, and only runs
   when the game is drawing into the default framebuffer.
3. **Per-frame flag reset on every swap** – `mWorldCapturedThisFrame` could stay `true`
   for many frames (stale world texture). It is now snapshotted and cleared in every swap,
   and set only if the capture really succeeded.
4. **Re-entrancy guard** – our own `glEnable/glDisable` calls no longer trigger the hooks.
5. **Clean/HUD history consistency** – each history texture is tagged (clean world vs. world+HUD);
   mixed pairs are no longer interpolated. A failed capture drops the history instead of
   pairing a stale texture with the next frame.
6. **Shader**
   - perspective-correct camera-rotation reprojection (was a linear UV shift with
     `fovH = fovV * aspect`, which is wrong away from the screen centre);
   - the edge/mismatch guard (`finalWeight`) was computed but never applied; it now blends
     between the real frame and the interpolated one;
   - HUD mask uses a threshold instead of a x8 linear ramp (blur on world edges was read as HUD);
   - phase 1.0 presents the real full-resolution frame untouched;
   - temporal smoothing capped at 0.15 and faded out while the camera moves.
7. `GlStateGuard` also saves/restores read/draw framebuffer and texture units 2 and 3.

Assumption: the game's reported FOV is the *vertical* FOV (gluPerspective convention).
