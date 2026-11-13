#include "GlRenderDiagnostics.hpp"

#if defined(__ANDROID__)

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <android/log.h>
#include <dlfcn.h>
#include <atomic>
#include <mutex>
#include <pl/memory/Hook.hpp>

namespace framegen {
namespace {

constexpr const char *TAG = "JNGS_GL_DIAG";
constexpr unsigned MAX_BIND_EVENTS = 100;
constexpr unsigned MAX_ATTACH_EVENTS = 100;
constexpr unsigned MAX_BLIT_EVENTS = 100;
constexpr unsigned MAX_EXECUTION_SUMMARIES = 4;

using BindFramebufferFn = void (GL_APIENTRY *)(GLenum, GLuint);
using FramebufferTexture2DFn =
    void (GL_APIENTRY *)(GLenum, GLenum, GLenum, GLuint, GLint);
// The GLES 3 core and ANGLE extension entry points have the same ABI.
using BlitFramebufferFn = void (GL_APIENTRY *)(
    GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint,
    GLbitfield, GLenum);

BindFramebufferFn originalBindFramebuffer = nullptr;
FramebufferTexture2DFn originalFramebufferTexture2D = nullptr;
BlitFramebufferFn originalBlitFramebuffer = nullptr;

pl::memory::HookHandle bindHook;
pl::memory::HookHandle attachmentHook;
pl::memory::HookHandle blitHook;

std::atomic<unsigned> bindEventCount{0};
std::atomic<unsigned> attachEventCount{0};
std::atomic<unsigned> blitEventCount{0};
std::atomic<unsigned> bindCalls{0};
std::atomic<unsigned> attachCalls{0};
std::atomic<unsigned> blitCalls{0};
void *glesHandle = nullptr;
std::atomic<bool> initialized{false};
std::mutex initMutex;

bool allowEvent(std::atomic<unsigned> &counter, unsigned limit) noexcept {
    return counter.fetch_add(1, std::memory_order_relaxed) < limit;
}

void logExecution(const char *name, std::atomic<unsigned> &counter) noexcept {
    const unsigned call = counter.fetch_add(1, std::memory_order_relaxed) + 1;
    if (call <= MAX_EXECUTION_SUMMARIES) {
        __android_log_print(ANDROID_LOG_INFO, TAG,
                            "HOOK_REACHED name=%s call=%u", name, call);
    }
}

void GL_APIENTRY bindFramebufferDetour(GLenum target, GLuint framebuffer) {
    logExecution("glBindFramebuffer", bindCalls);
    if (allowEvent(bindEventCount, MAX_BIND_EVENTS)) {
        __android_log_print(ANDROID_LOG_INFO, TAG,
                            "BIND target=0x%x framebuffer=%u",
                            target, framebuffer);
    }
    if (originalBindFramebuffer)
        originalBindFramebuffer(target, framebuffer);
}

void GL_APIENTRY framebufferTexture2DDetour(
    GLenum target, GLenum attachment, GLenum textarget,
    GLuint texture, GLint level) {
    logExecution("glFramebufferTexture2D", attachCalls);
    if (allowEvent(attachEventCount, MAX_ATTACH_EVENTS)) {
        __android_log_print(ANDROID_LOG_INFO, TAG,
                            "ATTACH target=0x%x attachment=0x%x "
                            "textureTarget=0x%x texture=%u level=%d",
                            target, attachment, textarget, texture, level);
    }
    if (originalFramebufferTexture2D)
        originalFramebufferTexture2D(target, attachment, textarget,
                                      texture, level);
}

void GL_APIENTRY blitFramebufferDetour(
    GLint sx0, GLint sy0, GLint sx1, GLint sy1,
    GLint dx0, GLint dy0, GLint dx1, GLint dy1,
    GLbitfield mask, GLenum filter) {
    logExecution("glBlitFramebuffer", blitCalls);
    if (allowEvent(blitEventCount, MAX_BLIT_EVENTS)) {
        __android_log_print(ANDROID_LOG_INFO, TAG,
                            "BLIT src=(%d,%d)-(%d,%d) "
                            "dst=(%d,%d)-(%d,%d) mask=0x%x filter=0x%x",
                            sx0, sy0, sx1, sy1,
                            dx0, dy0, dx1, dy1, mask, filter);
    }
    if (originalBlitFramebuffer)
        originalBlitFramebuffer(sx0, sy0, sx1, sy1,
                                dx0, dy0, dx1, dy1, mask, filter);
}

void *resolveBlitTarget(void *handle) noexcept {
    // Prefer the GLES core symbol, then try EGL's extension resolver.
    void *target = handle ? dlsym(handle, "glBlitFramebuffer") : nullptr;
    if (!target)
        target = reinterpret_cast<void *>(eglGetProcAddress("glBlitFramebuffer"));
    if (!target)
        target = reinterpret_cast<void *>(
            eglGetProcAddress("glBlitFramebufferANGLE"));
    return target;
}

} // namespace

void GlRenderDiagnostics::initialize() noexcept {
    std::lock_guard<std::mutex> lock(initMutex);

    if (eglGetCurrentContext() == EGL_NO_CONTEXT) {
        __android_log_print(ANDROID_LOG_WARN, TAG,
                            "INIT skipped: no current EGL context");
        return;
    }

    if (!glesHandle) {
        glesHandle = dlopen("libGLESv2.so", RTLD_NOW | RTLD_LOCAL);
        if (!glesHandle) {
            const char *error = dlerror();
            __android_log_print(ANDROID_LOG_ERROR, TAG,
                                "INIT failed: cannot open libGLESv2.so: %s",
                                error ? error : "unknown error");
            return;
        }
    }

    auto bindTarget = dlsym(glesHandle, "glBindFramebuffer");
    auto attachTarget = dlsym(glesHandle, "glFramebufferTexture2D");
    void *blitTarget = resolveBlitTarget(glesHandle);

    if (bindTarget && !bindHook.installed()) {
        bindHook = pl::memory::HookHandle(
            bindTarget,
            reinterpret_cast<pl::memory::FuncPtr>(&bindFramebufferDetour),
            reinterpret_cast<pl::memory::FuncPtr *>(&originalBindFramebuffer),
            pl::memory::HookPriority::Low);
    }

    if (attachTarget && !attachmentHook.installed()) {
        attachmentHook = pl::memory::HookHandle(
            attachTarget,
            reinterpret_cast<pl::memory::FuncPtr>(
                &framebufferTexture2DDetour),
            reinterpret_cast<pl::memory::FuncPtr *>(
                &originalFramebufferTexture2D),
            pl::memory::HookPriority::Low);
    }

    if (blitTarget && !blitHook.installed()) {
        blitHook = pl::memory::HookHandle(
            blitTarget,
            reinterpret_cast<pl::memory::FuncPtr>(&blitFramebufferDetour),
            reinterpret_cast<pl::memory::FuncPtr *>(&originalBlitFramebuffer),
            pl::memory::HookPriority::Low);
    }

    const bool bindInstalled = bindHook.installed();
    const bool attachInstalled = attachmentHook.installed();
    const bool blitInstalled = blitHook.installed();
    const bool anyInstalled = bindInstalled || attachInstalled || blitInstalled;

    __android_log_print(
        ANDROID_LOG_INFO, TAG,
        "HOOK_STATUS bind=%d attach=%d blit=%d blitTarget=%p",
        bindInstalled, attachInstalled, blitInstalled, blitTarget);

    // Partial success is useful: keep installed hooks active and retry only
    // missing hooks on a later swap. Do not make optional blitting mandatory.
    initialized.store(anyInstalled, std::memory_order_release);
    if (!anyInstalled) {
        __android_log_print(ANDROID_LOG_WARN, TAG,
                            "INIT incomplete: no GL hooks installed; retry next swap");
    } else if (!bindInstalled || !attachInstalled || !blitInstalled) {
        __android_log_print(ANDROID_LOG_WARN, TAG,
                            "INIT partial: successful hooks retained; missing hooks retried");
    }
}

void GlRenderDiagnostics::shutdown() noexcept {
    std::lock_guard<std::mutex> lock(initMutex);

    blitHook.reset();
    attachmentHook.reset();
    bindHook.reset();

    originalBlitFramebuffer = nullptr;
    originalFramebufferTexture2D = nullptr;
    originalBindFramebuffer = nullptr;
    if (glesHandle) {
        dlclose(glesHandle);
        glesHandle = nullptr;
    }
    initialized.store(false, std::memory_order_release);

    __android_log_print(
        ANDROID_LOG_INFO, TAG,
        "SHUTDOWN hooks removed; reached bind=%u attach=%u blit=%u",
        bindCalls.load(std::memory_order_relaxed),
        attachCalls.load(std::memory_order_relaxed),
        blitCalls.load(std::memory_order_relaxed));
}

} // namespace framegen

#else

namespace framegen {
// Host builds exercise the rest of FrameGen without Android EGL/GL hooks.
void GlRenderDiagnostics::initialize() noexcept {}
void GlRenderDiagnostics::shutdown() noexcept {}
} // namespace framegen

#endif
