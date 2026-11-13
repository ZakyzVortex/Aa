# GL diagnostics revision notes

This revision changes only `src/GlRenderDiagnostics.cpp`.

## Changes
- Hook installation status is reported independently for framebuffer binding, texture attachment, and blitting.
- Partial hook installation is retained; later calls can retry missing hooks without reinstalling successful ones.
- Blit resolution tries the GLES core `glBlitFramebuffer` symbol first, then the EGL core name, then `glBlitFramebufferANGLE`.
- Each detour reports a small number of `HOOK_REACHED` messages so hook execution can be distinguished from successful installation.
- Shutdown reports cumulative detour execution counts.
- The diagnostic remains read-only: it does not alter framebuffer bindings, attachments, blit parameters, or FrameGen's interpolation/capture path.

## Important limitations
- This does not identify the pre-HUD render target or prove world/UI composition order.
- `glBindFramebuffer` and `glFramebufferTexture2D` events alone cannot identify semantic render stages.
- The diagnostics are still initialized from the existing EGL swap callback. Therefore they begin observing GL calls only after the first successful swap callback reaches initialization.
- The core and ANGLE blit entry points may resolve to the same address. Only one blit hook is installed because resolution selects one target.
- Device-side installation and execution remain unverified until tested in Levi Launcher.
