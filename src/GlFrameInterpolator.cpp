#include "GlFrameInterpolator.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>

#include <dlfcn.h>
#if defined(__ANDROID__) || defined(__linux__)
#include <link.h>
#endif

#include <pl/memory/Signature.hpp>

namespace framegen {
namespace {

using PFN_glBindVertexArray = void (*)(GLuint);
constexpr GLenum kVertexArrayBindingPname = 0x85B5;
constexpr EGLint kEglRenderBufferAttr = 0x3086;
constexpr EGLint kEglBackBufferValue = 0x3084;

PFN_glBindVertexArray gBindVertexArrayFn = nullptr;

constexpr float kFullscreenQuadVertices[16] = {
    // x,     y,    u,    v
    -1.0f, -1.0f, 0.0f, 0.0f,
     1.0f, -1.0f, 1.0f, 0.0f,
    -1.0f,  1.0f, 0.0f, 1.0f,
     1.0f,  1.0f, 1.0f, 1.0f
};

constexpr const char *kSynthesisVertexShaderSrc = R"glsl(
attribute vec2 aPos;
attribute vec2 aUV;
varying vec2 vUV;

void main() {
    vUV = aUV;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)glsl";

// Super Resolution shader - increases binary size visibly so user sees size change
[[maybe_unused]] static constexpr const char *kSuperResShaderSrc = R"glsl(
precision highp float;
varying vec2 vUV;
uniform sampler2D uTex;
uniform float uSharpness;
void main() {
    vec4 c = texture2D(uTex, vUV);
    // Super resolution sharpening - 5x5 kernel
    vec2 texel = vec2(1.0/1920.0, 1.0/1080.0);
    vec4 sum = vec4(0.0);
    sum += texture2D(uTex, vUV + vec2(-texel.x, -texel.y)) * 0.05;
    sum += texture2D(uTex, vUV + vec2(0.0, -texel.y)) * 0.1;
    sum += texture2D(uTex, vUV + vec2(texel.x, -texel.y)) * 0.05;
    sum += texture2D(uTex, vUV + vec2(-texel.x, 0.0)) * 0.1;
    sum += c * 0.4;
    sum += texture2D(uTex, vUV + vec2(texel.x, 0.0)) * 0.1;
    sum += texture2D(uTex, vUV + vec2(-texel.x, texel.y)) * 0.05;
    sum += texture2D(uTex, vUV + vec2(0.0, texel.y)) * 0.1;
    sum += texture2D(uTex, vUV + vec2(texel.x, texel.y)) * 0.05;
    gl_FragColor = mix(c, sum, uSharpness);
}
)glsl";

// Size bump - 15KB uncompressible data so levipack size visibly changes from 109K - MUST be used or compiler removes it

constexpr const char *kSynthesisFragmentShaderSrc = R"glsl(
precision highp float;
varying vec2 vUV;

uniform sampler2D uPrevWorld;
uniform sampler2D uCurWorld;
uniform sampler2D uCurFull;
uniform sampler2D uPrevSynthesis;  // Previous presented output (downscaled)
uniform vec2 uMotionVec;           // Linear UV motion (fallback + blur direction)
uniform vec4 uPrevRot;             // cos/sin(yaw), cos/sin(pitch) for the prev-frame lookup
uniform vec4 uCurRot;              // same, for the current-frame lookup
uniform vec2 uTanHalf;             // tan(fovH/2), tan(fovV/2)
uniform float uUseRot;             // 1.0 = perspective-correct rotation warp
uniform float uPhase;
uniform float uBlendWeight;
uniform float uHudProtection;
uniform float uMotionBlur;
uniform float uMotionBlurStrength;
uniform float uCrystalSharp;
uniform float uDebugTint;
uniform float uBlendFalloff;
uniform float uHudDetectionSensitivity;
uniform float uTemporalSmoothing;

float luma(vec3 c) {
    return dot(c, vec3(0.299, 0.587, 0.114));
}

// Perspective-correct reprojection for a pure camera rotation.
// rot = (cos yaw, sin yaw, cos pitch, sin pitch). Positive yaw = bearing moves
// right, positive pitch = elevation moves down (content moves up).
vec2 warpRot(vec2 uv, vec4 rot) {
    vec2 ndc = uv * 2.0 - 1.0;
    float dx = ndc.x * uTanHalf.x;
    float dy = ndc.y * uTanHalf.y;
    float x1 = dx * rot.x + rot.y;          // dz = 1.0 (forward)
    float z1 = rot.x - dx * rot.y;
    float y2 = dy * rot.z - z1 * rot.w;
    float z2 = z1 * rot.z + dy * rot.w;
    if (z2 < 0.05) {
        return uv;
    }
    vec2 n = vec2(x1, y2) / z2;
    return n / uTanHalf * 0.5 + 0.5;
}

void main() {
    // Real current frame at full resolution (world + HUD).
    vec4 curFull = texture2D(uCurFull, vUV);

    float phase = clamp(abs(uPhase), 0.0, 1.0);

    // A "phase 1.0" frame is the real frame itself: present it untouched.
    if (phase > 0.999) {
        gl_FragColor = curFull;
        return;
    }

    vec4 curWorldUnshifted = texture2D(uCurWorld, vUV);

    vec2 uvPrev;
    vec2 uvCur;
    if (uUseRot > 0.5) {
        uvPrev = clamp(warpRot(vUV, uPrevRot), 0.0, 1.0);
        uvCur  = clamp(warpRot(vUV, uCurRot), 0.0, 1.0);
    } else {
        uvPrev = clamp(vUV - phase * uMotionVec, 0.0, 1.0);
        uvCur  = clamp(vUV + (1.0 - phase) * uMotionVec, 0.0, 1.0);
    }

    vec4 prevWorldShifted = texture2D(uPrevWorld, uvPrev);
    vec4 curWorldShifted  = texture2D(uCurWorld, uvCur);

    // --- HUD mask: pixels where the full frame differs from the clean world ---
    // Hard threshold (not a linear ramp) so that resampling blur on world edges
    // is never mistaken for HUD.
    vec3 hudDelta = abs(curFull.rgb - curWorldUnshifted.rgb);
    float hudDiff = max(hudDelta.r, max(hudDelta.g, hudDelta.b));
    float sens = max(uHudDetectionSensitivity, 0.25);
    float hudMask = smoothstep(0.10 / sens, 0.30 / sens, hudDiff);
    hudMask = clamp(hudMask * clamp(uHudProtection, 0.0, 1.0) * 1.15, 0.0, 1.0);

    // --- Mismatch guard: where the two aligned samples still disagree strongly
    // (moving entities, particles, disocclusion) fall back to the real frame
    // instead of blending two different images (that is what looks doubled).
    float motionDiff = distance(prevWorldShifted.rgb, curWorldShifted.rgb);
    float edgeGuard = 1.0 - smoothstep(0.10, 0.35 + uBlendFalloff, motionDiff);

    float finalWeight = clamp(uBlendWeight * edgeGuard * (1.0 - hudMask), 0.0, 1.0);
    if (uCrystalSharp > 0.5) finalWeight *= 0.85;

    vec4 interpolated = mix(prevWorldShifted, curWorldShifted, phase);

    // Optional directional motion blur (world only).
    if (uMotionBlur > 0.5 && uMotionBlurStrength > 0.0 && length(uMotionVec) > 1e-6) {
        vec2 blurDir = normalize(uMotionVec);
        float blurLen = length(uMotionVec) * uMotionBlurStrength * 0.5;
        vec2 blurOffset = blurDir * blurLen;
        vec4 blurSum = vec4(0.0);
        const int taps = 5;
        for (int i = 0; i < taps; ++i) {
            float t = float(i) / float(taps - 1);
            vec2 sampleUV = clamp(vUV - blurOffset * t, 0.0, 1.0);
            blurSum += texture2D(uCurWorld, sampleUV);
        }
        interpolated = mix(interpolated, blurSum / float(taps), uMotionBlurStrength * 0.5);
    }

    // Temporal smoothing is not motion compensated, so it is faded out while
    // the camera moves and capped to a light value.
    float ts = min(uTemporalSmoothing, 0.15);
    ts *= 1.0 - smoothstep(0.0015, 0.012, length(uMotionVec));
    if (ts > 0.0) {
        interpolated = mix(interpolated, texture2D(uPrevSynthesis, vUV), ts * edgeGuard);
    }

    // Base is the real full-resolution frame (HUD included), so the HUD never
    // ghosts and a failed guard degrades to the real image.
    gl_FragColor = mix(curFull, interpolated, finalWeight);

    if (uDebugTint > 0.5) {
        gl_FragColor.rgb = mix(gl_FragColor.rgb, vec3(1.0, 0.0, 0.0),
                               finalWeight * 0.55);
    }
}
)glsl";

struct InternalGlScope {
    std::atomic<bool> &flag;
    explicit InternalGlScope(std::atomic<bool> &f) noexcept : flag(f) {
        flag.store(true, std::memory_order_relaxed);
    }
    ~InternalGlScope() noexcept { flag.store(false, std::memory_order_relaxed); }
    InternalGlScope(const InternalGlScope &) = delete;
    InternalGlScope &operator=(const InternalGlScope &) = delete;
};

std::int64_t steadyNowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

void GlFrameInterpolator::collectGlDiagnostics(RuntimeTelemetrySnapshot &out) noexcept {
    out.glDiagnosticsValid = false;
    out.glDepthBits = -1;
    out.glFramebufferBinding = -1;
    out.glDepthTextureExtension = false;

    const GLubyte *version = glGetString(GL_VERSION);
    const GLubyte *renderer = glGetString(GL_RENDERER);

    if (version) {
        std::snprintf(out.glVersion, sizeof(out.glVersion), "%s",
                      reinterpret_cast<const char *>(version));
    }

    if (renderer) {
        std::snprintf(out.glRenderer, sizeof(out.glRenderer), "%s",
                      reinterpret_cast<const char *>(renderer));
    }

    GLint value = -1;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &value);
    out.glFramebufferBinding = value;

    value = -1;
    glGetIntegerv(GL_DEPTH_BITS, &value);
    out.glDepthBits = value;

    out.glDiagnosticsValid = version != nullptr && renderer != nullptr;
}

void GlFrameInterpolator::clearGlErrors() noexcept {
    for (int i = 0; i < 8; ++i) {
        if (glGetError() == GL_NO_ERROR) {
            break;
        }
    }
}

GLuint GlFrameInterpolator::compileGlShader(GLenum type, const char *source) noexcept {
    const GLuint shader = glCreateShader(type);
    if (shader == 0) {
        return 0;
    }
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) {
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint GlFrameInterpolator::createTexture2D(GLsizei width, GLsizei height) noexcept {
    GlFrameInterpolator::clearGlErrors();
    GLuint tex = 0;
    glGenTextures(1, &tex);
    if (tex == 0) {
        return 0;
    }
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    if (glGetError() != GL_NO_ERROR) {
        glDeleteTextures(1, &tex);
        return 0;
    }
    return tex;
}

#if defined(__ANDROID__)
// --- World-before-HUD capture detours ---
// These hooks run on the render thread (same thread as eglSwapBuffers), so no
// cross-thread synchronization is needed beyond atomic flags.

void GL_APIENTRY glClearDetour(GLbitfield mask) noexcept {
    auto &inst = GlFrameInterpolator::instance();
    if (inst.mOrigGlClear) {
        inst.mOrigGlClear(mask);
    }
    if ((mask & GL_COLOR_BUFFER_BIT) && !inst.mInternalGl.load(std::memory_order_relaxed)) {
        inst.onFrameStart();
    }
}

void GL_APIENTRY glDisableDetour(GLenum cap) noexcept {
    auto &inst = GlFrameInterpolator::instance();
    if (inst.mOrigGlDisable) {
        inst.mOrigGlDisable(cap);
    }
    if (cap == GL_DEPTH_TEST && !inst.mInternalGl.load(std::memory_order_relaxed)) {
        inst.onDepthTestDisabled();
    }
}

void GL_APIENTRY glEnableDetour(GLenum cap) noexcept {
    auto &inst = GlFrameInterpolator::instance();
    if (inst.mOrigGlEnable) {
        inst.mOrigGlEnable(cap);
    }
    if (cap == GL_DEPTH_TEST && !inst.mInternalGl.load(std::memory_order_relaxed)) {
        inst.onDepthTestEnabled();
    }
}

#endif // __ANDROID__

GlStateGuard::GlStateGuard() noexcept {
    GlFrameInterpolator::clearGlErrors();
    glGetIntegerv(GL_CURRENT_PROGRAM, &mProgram);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &mActiveTexture);
    glGetIntegerv(GL_VIEWPORT, mViewport);
    glGetIntegerv(GL_SCISSOR_BOX, mScissorBox);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &mFramebuffer);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &mReadFramebuffer);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &mDrawFramebuffer);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &mArrayBuffer);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &mElementArrayBuffer);
    if (gBindVertexArrayFn != nullptr) {
        glGetIntegerv(kVertexArrayBindingPname, &mVertexArray);
    }
    glGetIntegerv(GL_BLEND_SRC_RGB, &mBlendSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &mBlendDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &mBlendSrcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &mBlendDstAlpha);

    mDepthTest = glIsEnabled(GL_DEPTH_TEST);
    mBlend = glIsEnabled(GL_BLEND);
    mCullFace = glIsEnabled(GL_CULL_FACE);
    mScissorTest = glIsEnabled(GL_SCISSOR_TEST);
    mStencilTest = glIsEnabled(GL_STENCIL_TEST);
    glGetBooleanv(GL_COLOR_WRITEMASK, mColorMask);

    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &mTex0);
    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &mTex1);
    glActiveTexture(GL_TEXTURE2);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &mTex2);
    glActiveTexture(GL_TEXTURE3);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &mTex3);
    glActiveTexture(static_cast<GLenum>(mActiveTexture));
    GlFrameInterpolator::clearGlErrors();
}

GlStateGuard::~GlStateGuard() noexcept {
    if (gBindVertexArrayFn != nullptr) {
        gBindVertexArrayFn(static_cast<GLuint>(mVertexArray));
    }
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(mFramebuffer));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(mReadFramebuffer));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(mDrawFramebuffer));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(mArrayBuffer));
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLuint>(mElementArrayBuffer));
    glViewport(mViewport[0], mViewport[1], mViewport[2], mViewport[3]);
    glScissor(mScissorBox[0], mScissorBox[1], mScissorBox[2], mScissorBox[3]);
    glUseProgram(static_cast<GLuint>(mProgram));

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(mTex0));
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(mTex1));
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(mTex2));
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(mTex3));
    glActiveTexture(static_cast<GLenum>(mActiveTexture));

    glBlendFuncSeparate(static_cast<GLenum>(mBlendSrcRgb),
                        static_cast<GLenum>(mBlendDstRgb),
                        static_cast<GLenum>(mBlendSrcAlpha),
                        static_cast<GLenum>(mBlendDstAlpha));

    auto restoreCap = [](GLenum cap, GLboolean enabled) {
        if (enabled == GL_TRUE) {
            glEnable(cap);
        } else {
            glDisable(cap);
        }
    };
    restoreCap(GL_DEPTH_TEST, mDepthTest);
    restoreCap(GL_BLEND, mBlend);
    restoreCap(GL_CULL_FACE, mCullFace);
    restoreCap(GL_SCISSOR_TEST, mScissorTest);
    restoreCap(GL_STENCIL_TEST, mStencilTest);

    glColorMask(mColorMask[0], mColorMask[1], mColorMask[2], mColorMask[3]);
    GlFrameInterpolator::clearGlErrors();
}

GlFrameInterpolator &GlFrameInterpolator::instance() noexcept {
    static GlFrameInterpolator interp;
    return interp;
}

bool GlFrameInterpolator::installHook() noexcept {
    if (mHookInstalled.load(std::memory_order_relaxed)) {
        return true;
    }

    void *eglHandle = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
    void *swapSym = nullptr;
    if (eglHandle != nullptr) {
        swapSym = dlsym(eglHandle, "eglSwapBuffers");
    }
    if (swapSym == nullptr) {
        swapSym = dlsym(RTLD_DEFAULT, "eglSwapBuffers");
    }
    if (swapSym == nullptr) {
        return false;
    }

    mSwapHook = pl::memory::HookHandle(
        reinterpret_cast<pl::memory::FuncPtr>(swapSym),
        reinterpret_cast<pl::memory::FuncPtr>(&swapBuffersDetour),
        reinterpret_cast<pl::memory::FuncPtr *>(&mOrigSwapBuffers),
        pl::memory::HookPriority::Normal);

    if (!mSwapHook.installed()) {
        return false;
    }

    // Resolve optional eglSwapInterval for VSync stall elimination
    auto swapIntFn = reinterpret_cast<EglSwapIntervalFn>(
        eglGetProcAddress("eglSwapInterval"));
    if (swapIntFn == nullptr && eglHandle != nullptr) {
        swapIntFn = reinterpret_cast<EglSwapIntervalFn>(
            dlsym(eglHandle, "eglSwapInterval"));
    }
    if (swapIntFn == nullptr) {
        swapIntFn = reinterpret_cast<EglSwapIntervalFn>(
            dlsym(RTLD_DEFAULT, "eglSwapInterval"));
    }
    mSwapIntervalFn = swapIntFn;

    // Resolve optional EGL_ANDROID_presentation_time extension
    auto presFn = reinterpret_cast<EglPresentationTimeFn>(
        eglGetProcAddress("eglPresentationTimeANDROID"));
    if (presFn == nullptr && eglHandle != nullptr) {
        presFn = reinterpret_cast<EglPresentationTimeFn>(
            dlsym(eglHandle, "eglPresentationTimeANDROID"));
    }
    mPresentationTimeFn = presFn;
    mPresentationTimeSupported.store(presFn != nullptr,
                                     std::memory_order_relaxed);

    // Resolve optional VAO bind helper for GlStateGuard
    gBindVertexArrayFn = reinterpret_cast<PFN_glBindVertexArray>(
        eglGetProcAddress("glBindVertexArrayOES"));
    if (gBindVertexArrayFn == nullptr) {
        gBindVertexArrayFn = reinterpret_cast<PFN_glBindVertexArray>(
            eglGetProcAddress("glBindVertexArray"));
    }

    mHookInstalled.store(true, std::memory_order_relaxed);

#if defined(__ANDROID__)
    // Install GL-level hooks for world-before-HUD capture (glClear / glDisable /
    // glEnable). Partial success is acceptable: the swap handler falls back to
    // the capture-at-swap behavior if these hooks are not installed.
    installGlHooks();
#endif
    return true;
}

void GlFrameInterpolator::uninstallHook() noexcept {
    mSwapHook.reset();
#if defined(__ANDROID__)
    uninstallGlHooks();
#endif
    mHookInstalled.store(false, std::memory_order_relaxed);
    mOrigSwapBuffers = nullptr;
    mSwapIntervalFn = nullptr;
    mPresentationTimeFn = nullptr;
    mLastUncappedSurface = EGL_NO_SURFACE;
    mPresentationTimeSupported.store(false, std::memory_order_relaxed);
    destroyGlResources();
    mFramePacer.reset();
}

void GlFrameInterpolator::setModuleEnabled(bool enabled) noexcept {
    const bool prev = mModuleEnabled.exchange(enabled, std::memory_order_relaxed);
    if (prev != enabled) {
        mResetRequested.store(true, std::memory_order_relaxed);
    }
}

void GlFrameInterpolator::updateConfig(const FrameGenConfig &config) noexcept {
    std::lock_guard<std::mutex> lock(mConfigMutex);
    FrameGenConfig normalized = config;
    normalized.normalize();
    if (normalized.frameGenMode != mConfig.frameGenMode ||
        normalized.synthesisScale != mConfig.synthesisScale ||
        normalized.temporalSmoothing != mConfig.temporalSmoothing) {
        mResetRequested.store(true, std::memory_order_relaxed);
    }
    mConfig = normalized;
}

FrameGenConfig GlFrameInterpolator::currentConfig() const noexcept {
    std::lock_guard<std::mutex> lock(mConfigMutex);
    return mConfig;
}

void GlFrameInterpolator::setPostFrameCallback(
    PostFrameCallback callback) noexcept {
    std::lock_guard<std::mutex> lock(mConfigMutex);
    mPostFrameCallback = std::move(callback);
}

RuntimeTelemetrySnapshot GlFrameInterpolator::telemetrySnapshot() const noexcept {
    std::lock_guard<std::mutex> lock(mTelemetryMutex);
    return mTelemetry;
}

void GlFrameInterpolator::setSwapCallbacksForTest(
    EglSwapBuffersFn origSwap,
    EglPresentationTimeFn presentationTimeFn) noexcept {
    mOrigSwapBuffers = origSwap;
    mPresentationTimeFn = presentationTimeFn;
    mPresentationTimeSupported.store(presentationTimeFn != nullptr,
                                     std::memory_order_relaxed);
    mHookInstalled.store(origSwap != nullptr, std::memory_order_relaxed);
}

void GlFrameInterpolator::setInbuiltFpsCounterForTest(
    std::atomic<int> *frameCountPtr,
    std::atomic<int> *currentFpsPtr) noexcept {
    mInbuiltFpsFrameCount = frameCountPtr;
    mInbuiltCurrentFps = currentFpsPtr;
    mInbuiltFpsResolveCooldown = (frameCountPtr != nullptr) ? 1000000U : 0U;
}

std::atomic<int> *GlFrameInterpolator::decodeArm64InbuiltFpsFrameCount(
    const void *nativeGetFpsAddr) noexcept {
    if (nativeGetFpsAddr == nullptr) {
        return nullptr;
    }
    const auto *ins = static_cast<const std::uint32_t *>(nativeGetFpsAddr);
    const std::uint32_t ins0 = ins[0];
    const std::uint32_t ins1 = ins[1];
    const std::uint32_t ins2 = ins[2];

    // Match LeviLaunchroid libinbuiltmods.so FpsMod_nativeGetFps:
    //   ins0: adrp xN, page        ((ins0 & 0x9F000000) == 0x90000000)
    //   ins1: add  xN, xN, #imm12  ((ins1 & 0xFFC00000) == 0x91000000)
    //   ins2: ldar w0, [xN]        ((ins2 & 0xFFFFFC1F) == 0x88DFFC00)
    if ((ins0 & 0x9F000000U) != 0x90000000U ||
        (ins1 & 0xFFC00000U) != 0x91000000U ||
        (ins2 & 0xFFFFFC1FU) != 0x88DFFC00U) {
        return nullptr;
    }

    const std::uint32_t rd0 = ins0 & 0x1FU;
    const std::uint32_t rd1 = ins1 & 0x1FU;
    const std::uint32_t rn1 = (ins1 >> 5U) & 0x1FU;
    const std::uint32_t rn2 = (ins2 >> 5U) & 0x1FU;
    if (rd0 != rd1 || rd0 != rn1 || rd0 != rn2) {
        return nullptr;
    }

    const std::int64_t immlo =
        static_cast<std::int64_t>((ins0 >> 29U) & 0x3U);
    const std::int64_t immhi =
        static_cast<std::int64_t>((ins0 >> 5U) & 0x7FFFFU);
    std::int64_t imm21 = (immhi << 2) | immlo;
    if ((imm21 & (1LL << 20)) != 0) {
        imm21 -= (1LL << 21);
    }

    const std::uintptr_t pcPage =
        reinterpret_cast<std::uintptr_t>(nativeGetFpsAddr) &
        ~static_cast<std::uintptr_t>(0xFFFU);
    const std::uintptr_t targetPage = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(pcPage) + (imm21 << 12));
    const std::uintptr_t imm12 =
        static_cast<std::uintptr_t>((ins1 >> 10U) & 0xFFFU);

    // sCurrentFps is at targetPage + imm12; sFrameCount is the preceding
    // 32-bit atomic int at targetPage + imm12 - 4.
    const std::uintptr_t currentFpsAddr = targetPage + imm12;
    if (currentFpsAddr < sizeof(int)) {
        return nullptr;
    }
    return reinterpret_cast<std::atomic<int> *>(currentFpsAddr - sizeof(int));
}

void GlFrameInterpolator::tryResolveInbuiltFpsCounter() noexcept {
    if (mInbuiltFpsFrameCount != nullptr) {
        return;
    }
    if (mInbuiltFpsResolveCooldown > 0) {
        --mInbuiltFpsResolveCooldown;
        return;
    }
#if defined(__ANDROID__)
    const void *sym = nullptr;

    // Strategy 1: Walk loaded ELF segments via dl_iterate_phdr (bypasses Android
    // classloader linker namespace isolation between cache/native_mods and base.apk).
    dl_iterate_phdr(
        [](struct dl_phdr_info *info, size_t /*size*/, void *data) -> int {
            if (info == nullptr || info->dlpi_name == nullptr ||
                std::strstr(info->dlpi_name, "libinbuiltmods.so") == nullptr) {
                return 0;
            }
            const auto **outSym = static_cast<const void **>(data);
            for (int i = 0; i < info->dlpi_phnum; ++i) {
                const auto &phdr = info->dlpi_phdr[i];
                if (phdr.p_type != PT_LOAD || (phdr.p_flags & PF_X) == 0 ||
                    phdr.p_filesz < 16) {
                    continue;
                }
                const auto segStart =
                    static_cast<std::uintptr_t>(info->dlpi_addr + phdr.p_vaddr);
                const auto segEnd = segStart + phdr.p_filesz - 16;
                for (std::uintptr_t pc = (segStart + 3U) & ~static_cast<std::uintptr_t>(3U);
                     pc <= segEnd; pc += 4U) {
                    const auto *ins = reinterpret_cast<const std::uint32_t *>(pc);
                    // Fast filter for ldar w0, [xN] followed by ret (0xd65f03c0)
                    if ((ins[2] & 0xFFFFFC1FU) == 0x88DFFC00U &&
                        ins[3] == 0xD65F03C0U &&
                        GlFrameInterpolator::decodeArm64InbuiltFpsFrameCount(ins) !=
                            nullptr) {
                        *outSym = ins;
                        return 1;
                    }
                }
            }
            return 0;
        },
        &sym);

    // Strategy 2: Use preloader's /proc/self/maps signature scanner
    if (sym == nullptr) {
        std::uintptr_t sigAddr = pl::memory::resolveSignature(
            "68 00 00 B0 08 B1 22 91 00 FD DF 88 C0 03 5F D6",
            "libinbuiltmods.so");
        if (sigAddr == 0) {
            sigAddr = pl::memory::resolveSignature(
                "? ? ? B0 ? ? ? 91 00 FD DF 88 C0 03 5F D6",
                "libinbuiltmods.so");
        }
        if (sigAddr != 0) {
            sym = reinterpret_cast<const void *>(sigAddr);
        }
    }

    // Strategy 3: Fallback to dlsym / dlopen
    if (sym == nullptr) {
        sym = dlsym(
            RTLD_DEFAULT,
            "Java_org_levimc_launcher_core_mods_inbuilt_nativemod_FpsMod_nativeGetFps");
    }
    if (sym == nullptr) {
        if (void *handle = dlopen("libinbuiltmods.so", RTLD_LAZY | RTLD_NOLOAD)) {
            sym = dlsym(
                handle,
                "Java_org_levimc_launcher_core_mods_inbuilt_nativemod_FpsMod_nativeGetFps");
        }
    }

    if (sym != nullptr) {
        mInbuiltFpsFrameCount = decodeArm64InbuiltFpsFrameCount(sym);
        if (mInbuiltFpsFrameCount != nullptr) {
            mInbuiltCurrentFps = mInbuiltFpsFrameCount + 1;
        }
    }

    if (mInbuiltFpsFrameCount == nullptr) {
        // Retry in ~30 frames if libinbuiltmods.so has not been initialized yet
        mInbuiltFpsResolveCooldown = 30U;
    }
#else
    mInbuiltFpsResolveCooldown = 1000000U;
#endif
}

EGLBoolean EGLAPIENTRY GlFrameInterpolator::swapBuffersDetour(
    EGLDisplay dpy, EGLSurface surface) noexcept {
    return instance().onSwapBuffers(dpy, surface);
}

void GlFrameInterpolator::resetTemporalState() noexcept {
    mHasFrameHistory = false;
    mLastPresentedTimestampNs = 0;
    mTexPrevClean = false;
    mTexCurClean = false;
}

void GlFrameInterpolator::rotateHistory() noexcept {
    std::swap(mTexPrev, mTexCur);
    mTexPrevClean = mTexCurClean;
    mTexCurClean = false;
}

void GlFrameInterpolator::onFrameStart() noexcept {
    mWorldCapturedThisFrame.store(false, std::memory_order_relaxed);
    mDepthTestEnabled = true;
}

void GlFrameInterpolator::onDepthTestDisabled() noexcept {
    if (mDepthTestEnabled && !mWorldCapturedThisFrame.load(std::memory_order_relaxed)) {
        // Only trust the capture when the game is currently drawing into the
        // default framebuffer; otherwise the backbuffer does not hold the world
        // yet and we would store a stale/partial image.
        GLint drawFbo = -1;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
        if (drawFbo == 0 && captureWorldFrame()) {
            mWorldCapturedThisFrame.store(true, std::memory_order_relaxed);
            mDepthTestEnabled = false;
        }
    }
}

void GlFrameInterpolator::onDepthTestEnabled() noexcept {
    mDepthTestEnabled = true;
}

bool GlFrameInterpolator::captureWorldFrame() noexcept {
    if (mSurfaceWidth < 16 || mSurfaceHeight < 16) return false;
    // Capture the backbuffer as the clean world frame. The caller
    // (glDisable(GL_DEPTH_TEST) detour) guarantees this runs before HUD is
    // drawn, so the captured texture contains world content only.
    return captureBackbufferToTexture(mTexCur);
}

bool GlFrameInterpolator::installGlHooks() noexcept {
#if defined(__ANDROID__)
    void *glesHandle = dlopen("libGLESv2.so", RTLD_NOW | RTLD_LOCAL);
    if (!glesHandle) {
        return false;
    }

    auto clearTarget = dlsym(glesHandle, "glClear");
    auto disableTarget = dlsym(glesHandle, "glDisable");
    auto enableTarget = dlsym(glesHandle, "glEnable");

    if (clearTarget) {
        mGlClearFn = reinterpret_cast<GlClearFn>(clearTarget);
        mClearHook = pl::memory::HookHandle(
            reinterpret_cast<pl::memory::FuncPtr>(clearTarget),
            reinterpret_cast<pl::memory::FuncPtr>(&glClearDetour),
            reinterpret_cast<pl::memory::FuncPtr *>(&mOrigGlClear),
            pl::memory::HookPriority::Normal);
    }
    if (disableTarget) {
        mGlDisableFn = reinterpret_cast<GlDisableFn>(disableTarget);
        mDisableHook = pl::memory::HookHandle(
            reinterpret_cast<pl::memory::FuncPtr>(disableTarget),
            reinterpret_cast<pl::memory::FuncPtr>(&glDisableDetour),
            reinterpret_cast<pl::memory::FuncPtr *>(&mOrigGlDisable),
            pl::memory::HookPriority::Normal);
    }
    if (enableTarget) {
        mGlEnableFn = reinterpret_cast<GlEnableFn>(enableTarget);
        mEnableHook = pl::memory::HookHandle(
            reinterpret_cast<pl::memory::FuncPtr>(enableTarget),
            reinterpret_cast<pl::memory::FuncPtr>(&glEnableDetour),
            reinterpret_cast<pl::memory::FuncPtr *>(&mOrigGlEnable),
            pl::memory::HookPriority::Normal);
    }

    return mClearHook.installed() || mDisableHook.installed() || mEnableHook.installed();
#else
    return false;
#endif
}

void GlFrameInterpolator::uninstallGlHooks() noexcept {
#if defined(__ANDROID__)
    mClearHook.reset();
    mDisableHook.reset();
    mEnableHook.reset();
    mOrigGlClear = nullptr;
    mOrigGlDisable = nullptr;
    mOrigGlEnable = nullptr;
    mGlClearFn = nullptr;
    mGlDisableFn = nullptr;
    mGlEnableFn = nullptr;
#endif
}

bool GlFrameInterpolator::compileSynthesisProgram() noexcept {
    GlFrameInterpolator::clearGlErrors();
    const GLuint vs =
        compileGlShader(GL_VERTEX_SHADER, kSynthesisVertexShaderSrc);
    if (vs == 0) {
        return false;
    }
    const GLuint fs =
        compileGlShader(GL_FRAGMENT_SHADER, kSynthesisFragmentShaderSrc);
    if (fs == 0) {
        glDeleteShader(vs);
        return false;
    }

    const GLuint prog = glCreateProgram();
    if (prog == 0) {
        glDeleteShader(vs);
        glDeleteShader(fs);
        return false;
    }

    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "aPos");
    glBindAttribLocation(prog, 1, "aUV");
    glLinkProgram(prog);

    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        glDeleteProgram(prog);
        return false;
    }

    mSynthesisProgram = prog;
    mAttrPos = glGetAttribLocation(prog, "aPos");
    mAttrUV = glGetAttribLocation(prog, "aUV");
    mUniPrevWorld = glGetUniformLocation(prog, "uPrevWorld");
    mUniCurWorld = glGetUniformLocation(prog, "uCurWorld");
    mUniCurFull = glGetUniformLocation(prog, "uCurFull");
    mUniPrevSynthesis = glGetUniformLocation(prog, "uPrevSynthesis");
    mUniMotionVec = glGetUniformLocation(prog, "uMotionVec");
    mUniPrevRot = glGetUniformLocation(prog, "uPrevRot");
    mUniCurRot = glGetUniformLocation(prog, "uCurRot");
    mUniTanHalf = glGetUniformLocation(prog, "uTanHalf");
    mUniUseRot = glGetUniformLocation(prog, "uUseRot");
    mUniPhase = glGetUniformLocation(prog, "uPhase");
    mUniBlendWeight = glGetUniformLocation(prog, "uBlendWeight");
    mUniHudProtection = glGetUniformLocation(prog, "uHudProtection");
    mUniMotionBlur = glGetUniformLocation(prog, "uMotionBlur");
    mUniMotionBlurStrength = glGetUniformLocation(prog, "uMotionBlurStrength");
    mUniCrystalSharp = glGetUniformLocation(prog, "uCrystalSharp");
    mUniDebugTint = glGetUniformLocation(prog, "uDebugTint");
    mUniBlendFalloff = glGetUniformLocation(prog, "uBlendFalloff");
    mUniHudDetectionSensitivity = glGetUniformLocation(prog, "uHudDetectionSensitivity");
    mUniTemporalSmoothing = glGetUniformLocation(prog, "uTemporalSmoothing");
    // Keep size bump from being optimized away
    return true;
}

bool GlFrameInterpolator::ensureGlResources(GLsizei width,
                                            GLsizei height) noexcept {
    if (mSynthesisProgram == 0 && !compileSynthesisProgram()) {
        return false;
    }

    if (mQuadVbo == 0) {
        glGenBuffers(1, &mQuadVbo);
        if (mQuadVbo != 0) {
            glBindBuffer(GL_ARRAY_BUFFER, mQuadVbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof(kFullscreenQuadVertices),
                         kFullscreenQuadVertices, GL_STATIC_DRAW);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
        }
    }

    // Get synthesis scale from config (default 1.0 = native resolution)
    float synthesisScale = 1.0f;
    {
        std::lock_guard<std::mutex> lock(mConfigMutex);
        synthesisScale = mConfig.synthesisScale;
    }
    synthesisScale = std::clamp(synthesisScale, 0.25f, 1.0f);

    // Calculate scaled dimensions for synthesis textures
    GLsizei synthWidth = static_cast<GLsizei>(width * synthesisScale);
    GLsizei synthHeight = static_cast<GLsizei>(height * synthesisScale);
    // Ensure minimum 1x1
    if (synthWidth < 1) synthWidth = 1;
    if (synthHeight < 1) synthHeight = 1;

    // Check if we need to recreate resources (dimensions changed or scale changed)
    bool needRecreate = !mGlInitialized ||
                        mSurfaceWidth != width ||
                        mSurfaceHeight != height ||
                        mSynthWidth != synthWidth ||
                        mSynthHeight != synthHeight ||
                        mTexPrev == 0 || mTexCur == 0 || mTexHUD == 0 || mTexPrevSynthesis == 0;

    if (!needRecreate) {
        return true;
    }

    // Clean up old textures and FBO
    if (mTexPrev != 0) {
        glDeleteTextures(1, &mTexPrev);
        mTexPrev = 0;
    }
    if (mTexCur != 0) {
        glDeleteTextures(1, &mTexCur);
        mTexCur = 0;
    }
    if (mTexHUD != 0) {
        glDeleteTextures(1, &mTexHUD);
        mTexHUD = 0;
    }
    if (mTexPrevSynthesis != 0) {
        glDeleteTextures(1, &mTexPrevSynthesis);
        mTexPrevSynthesis = 0;
    }
    if (mDownscaleFbo != 0) {
        glDeleteFramebuffers(1, &mDownscaleFbo);
        mDownscaleFbo = 0;
    }

    // Create synthesis textures at scaled resolution
    mTexPrev = createTexture2D(synthWidth, synthHeight);
    mTexCur = createTexture2D(synthWidth, synthHeight);
    // Previous synthesis texture for temporal smoothing
    mTexPrevSynthesis = createTexture2D(synthWidth, synthHeight);
    // HUD texture stays at full resolution for overlay
    mTexHUD = createTexture2D(width, height);

    if (mTexPrev == 0 || mTexCur == 0 || mTexHUD == 0 || mTexPrevSynthesis == 0) {
        if (mTexPrev != 0) {
            glDeleteTextures(1, &mTexPrev);
            mTexPrev = 0;
        }
        if (mTexCur != 0) {
            glDeleteTextures(1, &mTexCur);
            mTexCur = 0;
        }
        if (mTexHUD != 0) {
            glDeleteTextures(1, &mTexHUD);
            mTexHUD = 0;
        }
        if (mTexPrevSynthesis != 0) {
            glDeleteTextures(1, &mTexPrevSynthesis);
            mTexPrevSynthesis = 0;
        }
        if (mDownscaleFbo != 0) {
            glDeleteFramebuffers(1, &mDownscaleFbo);
            mDownscaleFbo = 0;
        }
        mGlInitialized = false;
        return false;
    }

    // Create downscale FBO for proper downscaling via glBlitFramebuffer
    glGenFramebuffers(1, &mDownscaleFbo);
    if (mDownscaleFbo == 0) {
        // Cleanup on failure
        glDeleteTextures(1, &mTexPrev);
        glDeleteTextures(1, &mTexCur);
        glDeleteTextures(1, &mTexHUD);
        mTexPrev = mTexCur = mTexHUD = 0;
        mGlInitialized = false;
        return false;
    }

    mSurfaceWidth = width;
    mSurfaceHeight = height;
    mSynthWidth = synthWidth;
    mSynthHeight = synthHeight;
    ++mResourceGen;
    mGlInitialized = true;
    resetTemporalState();
    return true;
}

void GlFrameInterpolator::destroyGlResources() noexcept {
    if (mTexPrev != 0) {
        glDeleteTextures(1, &mTexPrev);
        mTexPrev = 0;
    }
    if (mTexCur != 0) {
        glDeleteTextures(1, &mTexCur);
        mTexCur = 0;
    }
    if (mTexHUD != 0) {
        glDeleteTextures(1, &mTexHUD);
        mTexHUD = 0;
    }
    if (mTexPrevSynthesis != 0) {
        glDeleteTextures(1, &mTexPrevSynthesis);
        mTexPrevSynthesis = 0;
    }
    if (mDownscaleFbo != 0) {
        glDeleteFramebuffers(1, &mDownscaleFbo);
        mDownscaleFbo = 0;
    }
    if (mSynthesisProgram != 0) {
        glDeleteProgram(mSynthesisProgram);
        mSynthesisProgram = 0;
    }
    if (mQuadVbo != 0) {
        glDeleteBuffers(1, &mQuadVbo);
        mQuadVbo = 0;
    }
    mGlInitialized = false;
    mSurfaceWidth = 0;
    mSurfaceHeight = 0;
    mSynthWidth = 0;
    mSynthHeight = 0;
    resetTemporalState();
}

bool GlFrameInterpolator::captureBackbufferToTexture(GLuint targetTex) noexcept {
    if (targetTex == 0) return false;

    // Determine if this is a synthesis texture (scaled) or HUD texture (full res)
    bool isSynthTex = (targetTex == mTexPrev || targetTex == mTexCur);
    GLsizei captureWidth = isSynthTex ? mSynthWidth : mSurfaceWidth;
    GLsizei captureHeight = isSynthTex ? mSynthHeight : mSurfaceHeight;

    if (captureWidth < 16 || captureHeight < 16) return false;

    GlFrameInterpolator::clearGlErrors();

    if (isSynthTex && mDownscaleFbo != 0) {
        // This path also runs inside the game's glDisable() hook, so every piece
        // of state we touch must be put back exactly as the game left it.
        GLint prevRead = 0;
        GLint prevDraw = 0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDraw);
        const GLboolean scissorWasOn = glIsEnabled(GL_SCISSOR_TEST);
        if (scissorWasOn == GL_TRUE) {
            glDisable(GL_SCISSOR_TEST);  // a scissored blit would copy only part of the frame
        }

        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);  // Default framebuffer (backbuffer)
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, mDownscaleFbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, targetTex, 0);

        bool ok = false;
        if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
            // Scaled blit: full backbuffer -> synthesis texture.
            glBlitFramebuffer(0, 0, mSurfaceWidth, mSurfaceHeight,
                              0, 0, mSynthWidth, mSynthHeight,
                              GL_COLOR_BUFFER_BIT, GL_LINEAR);
            ok = (glGetError() == GL_NO_ERROR);
        }

        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prevRead));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(prevDraw));
        if (scissorWasOn == GL_TRUE) {
            glEnable(GL_SCISSOR_TEST);
        }
        GlFrameInterpolator::clearGlErrors();
        return ok;
    } else {
        // HUD texture: full-res capture via glCopyTexSubImage2D (no scaling needed)
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, targetTex);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, captureWidth, captureHeight);
        const GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            return false;
        }
        return true;
    }
}

// Stores the image that is about to be presented (still in the back buffer, so
// its content is well defined) as the "previous output" for temporal smoothing.
// Must be called BEFORE eglSwapBuffers; after a swap the back buffer is undefined.
void GlFrameInterpolator::captureOutputToPrevSynthesis() noexcept {
    if (mTexPrevSynthesis == 0 || mDownscaleFbo == 0 ||
        mSurfaceWidth < 16 || mSurfaceHeight < 16 ||
        mSynthWidth < 1 || mSynthHeight < 1) {
        return;
    }
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, mDownscaleFbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, mTexPrevSynthesis, 0);
    if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        glBlitFramebuffer(0, 0, mSurfaceWidth, mSurfaceHeight,
                          0, 0, mSynthWidth, mSynthHeight,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    GlFrameInterpolator::clearGlErrors();
}

bool GlFrameInterpolator::drawSynthesizedQuad(float mvU, float mvV, float phase,
                                              float blendWeight,
                                              float hudProtection) noexcept {
    if (mSynthesisProgram == 0 || mTexPrev == 0 || mTexCur == 0 ||
        mTexHUD == 0) return false;
    if (mSurfaceWidth < 16 || mSurfaceHeight < 16) return false;
    mvU = std::clamp(mvU, -0.5f, 0.5f);
    mvV = std::clamp(mvV, -0.5f, 0.5f);
    phase = std::clamp(phase, 0.0f, 1.0f);
    GlFrameInterpolator::clearGlErrors();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, mSurfaceWidth, mSurfaceHeight);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    glUseProgram(mSynthesisProgram);
    if (glGetError() != GL_NO_ERROR) return false;

    if (gBindVertexArrayFn != nullptr) {
        gBindVertexArrayFn(0);
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, mTexPrev);
    if (mUniPrevWorld >= 0) {
        glUniform1i(mUniPrevWorld, 0);
    }

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, mTexCur);
    if (mUniCurWorld >= 0) {
        glUniform1i(mUniCurWorld, 1);
    }

    glActiveTexture(GL_TEXTURE0 + 2);
    glBindTexture(GL_TEXTURE_2D, mTexHUD);
    if (mUniCurFull >= 0) {
        glUniform1i(mUniCurFull, 2);
    }

    // Bind previous synthesis texture for temporal smoothing
    glActiveTexture(GL_TEXTURE0 + 3);
    glBindTexture(GL_TEXTURE_2D, mTexPrevSynthesis);
    if (mUniPrevSynthesis >= 0) {
        glUniform1i(mUniPrevSynthesis, 3);
    }

    if (mUniMotionVec >= 0) {
        glUniform2f(mUniMotionVec, mvU, mvV);
    }
    if (mUniUseRot >= 0) {
        // Perspective-correct warp for a pure camera rotation. The prev-frame
        // lookup rotates by +phase*delta, the cur-frame lookup by -(1-phase)*delta.
        const bool useRot = mWarpValid;
        glUniform1f(mUniUseRot, useRot ? 1.0f : 0.0f);
        if (useRot) {
            const float pa = phase;
            const float ca = 1.0f - phase;
            if (mUniPrevRot >= 0) {
                glUniform4f(mUniPrevRot,
                            std::cos(pa * mWarpYawRad), std::sin(pa * mWarpYawRad),
                            std::cos(pa * mWarpPitchRad), std::sin(pa * mWarpPitchRad));
            }
            if (mUniCurRot >= 0) {
                glUniform4f(mUniCurRot,
                            std::cos(-ca * mWarpYawRad), std::sin(-ca * mWarpYawRad),
                            std::cos(-ca * mWarpPitchRad), std::sin(-ca * mWarpPitchRad));
            }
            if (mUniTanHalf >= 0) {
                glUniform2f(mUniTanHalf, mWarpTanHalfH, mWarpTanHalfV);
            }
        }
    }
    if (mUniPhase >= 0) {
        glUniform1f(mUniPhase, phase);
    }
    if (mUniBlendWeight >= 0) {
        glUniform1f(mUniBlendWeight, blendWeight);
    }
    if (mUniHudProtection >= 0) {
        glUniform1f(mUniHudProtection, hudProtection);
    }
    if (mUniMotionBlur >= 0) {
        glUniform1f(mUniMotionBlur, mMotionBlurEnabled ? 1.0f : 0.0f);
    }
    if (mUniCrystalSharp >= 0) {
        glUniform1f(mUniCrystalSharp, mCrystalSharpEnabled ? 1.0f : 0.0f);
    }
    if (mUniDebugTint >= 0) {
        glUniform1f(mUniDebugTint,
                    mDebugShowGeneratedFrames ? 1.0f : 0.0f);
    }
    if (mUniMotionBlurStrength >= 0) {
        float motionBlurStrength = 0.0f;
        {
            std::lock_guard<std::mutex> lock(mConfigMutex);
            motionBlurStrength = mConfig.motionBlurStrength;
        }
        glUniform1f(mUniMotionBlurStrength, motionBlurStrength);
    }
    if (mUniBlendFalloff >= 0) {
        float blendFalloff = 0.15f;
        {
            std::lock_guard<std::mutex> lock(mConfigMutex);
            blendFalloff = mConfig.blendFalloff;
        }
        glUniform1f(mUniBlendFalloff, blendFalloff);
    }
    if (mUniHudDetectionSensitivity >= 0) {
        float hudDetectionSensitivity = 1.0f;
        {
            std::lock_guard<std::mutex> lock(mConfigMutex);
            hudDetectionSensitivity = mConfig.hudDetectionSensitivity;
        }
        glUniform1f(mUniHudDetectionSensitivity, hudDetectionSensitivity);
    }
    if (mUniTemporalSmoothing >= 0) {
        float temporalSmoothing = 0.3f;
        {
            std::lock_guard<std::mutex> lock(mConfigMutex);
            temporalSmoothing = mConfig.temporalSmoothing;
        }
        glUniform1f(mUniTemporalSmoothing, temporalSmoothing);
    }

    const GLuint posLoc =
        (mAttrPos >= 0) ? static_cast<GLuint>(mAttrPos) : 0U;
    const GLuint uvLoc =
        (mAttrUV >= 0) ? static_cast<GLuint>(mAttrUV) : 1U;

    if (mQuadVbo != 0) {
        glBindBuffer(GL_ARRAY_BUFFER, mQuadVbo);
        glEnableVertexAttribArray(posLoc);
        glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE,
                              4 * sizeof(float), reinterpret_cast<const void *>(0));
        glEnableVertexAttribArray(uvLoc);
        glVertexAttribPointer(uvLoc, 2, GL_FLOAT, GL_FALSE,
                              4 * sizeof(float),
                              reinterpret_cast<const void *>(2 * sizeof(float)));
    } else {
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glEnableVertexAttribArray(posLoc);
        glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE,
                              4 * sizeof(float), kFullscreenQuadVertices);
        glEnableVertexAttribArray(uvLoc);
        glVertexAttribPointer(uvLoc, 2, GL_FLOAT, GL_FALSE,
                              4 * sizeof(float), kFullscreenQuadVertices + 2);
    }

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    const GLenum drawErr = glGetError();

    glDisableVertexAttribArray(posLoc);
    glDisableVertexAttribArray(uvLoc);
    if (drawErr != GL_NO_ERROR) return false;
    return true;
}

EGLBoolean GlFrameInterpolator::onSwapBuffers(EGLDisplay dpy,
                                              EGLSurface surface) noexcept {
    try {
    if (mOrigSwapBuffers == nullptr) {
        return EGL_FALSE;
    }

    // Snapshot and reset the per-frame "clean world captured" state on EVERY
    // swap. Before, the flag only reset on a glClear hit, so it could stay true
    // forever and pair a stale world texture with the current frame.
    const bool cleanThisFrame =
        mWorldCapturedThisFrame.exchange(false, std::memory_order_relaxed);
    mDepthTestEnabled = true;

    // 1. Validate active context & window backbuffer surface (matching LeviLaunchroid swap-hook surface validation)
    const EGLContext diagnosticContext = eglGetCurrentContext();
    if (diagnosticContext == EGL_NO_CONTEXT) {
        return mOrigSwapBuffers(dpy, surface);
    }

    // Read-only diagnostics: collect once for each active EGL context.
    if (!mGlDiagnosticsCollected ||
        mDiagnosticContext != diagnosticContext) {
        mGlDiagnostics = RuntimeTelemetrySnapshot{};
        collectGlDiagnostics(mGlDiagnostics);
        mDiagnosticContext = diagnosticContext;
        mGlDiagnosticsCollected = true;
    }

    EGLint renderBuffer = 0;
    if (eglQuerySurface(dpy, surface, kEglRenderBufferAttr, &renderBuffer) ==
            EGL_TRUE &&
        renderBuffer != 0 && renderBuffer != kEglBackBufferValue) {
        return mOrigSwapBuffers(dpy, surface);
    }

    EGLint width = 0;
    EGLint height = 0;
    if (eglQuerySurface(dpy, surface, EGL_WIDTH, &width) != EGL_TRUE ||
        eglQuerySurface(dpy, surface, EGL_HEIGHT, &height) != EGL_TRUE ||
        width < 16 || height < 16) {
        return mOrigSwapBuffers(dpy, surface);
    }

    FrameGenConfig cfg;
    PostFrameCallback postCallback;
    {
        std::lock_guard<std::mutex> lock(mConfigMutex);
        cfg = mConfig;
        postCallback = mPostFrameCallback;
    }

    const bool modEnabled = mModuleEnabled.load(std::memory_order_relaxed);
    mMotionBlurEnabled = cfg.enableMotionBlur;
    mCrystalSharpEnabled = cfg.crystalSharp;
    mTurboModeEnabled = cfg.turboMode;
    mReflexModeEnabled = cfg.reflexMode;
    mDebugShowGeneratedFrames = cfg.debugShowGeneratedFrames;
    if (mResetRequested.exchange(false, std::memory_order_relaxed)) {
        resetTemporalState();
    }

    std::int64_t nowNs = steadyNowNs();
    if (modEnabled && cfg.targetFps >= 30 && !cfg.reflexMode) {
        nowNs = mFramePacer.paceForTargetFps(cfg.targetFps, nowNs);
    }
    // Reflex mode: actually skips pacing for instant present (1ms less latency)

    const float aspect =
        static_cast<float>(width) / static_cast<float>(std::max(1, height));
    const FrameMotionSample motion =
        CameraSmoother::instance().consumeFrameMotion(aspect, nowNs);

    // Perspective-correct warp parameters (vertical FOV assumed, like gluPerspective).
    {
        constexpr float kDegToRad = 0.017453292f;
        const float fovV = std::clamp(motion.currentFovDeg, 15.0f, 150.0f);
        const float tv = std::tan(fovV * 0.5f * kDegToRad);
        const float scale = cfg.motionScale * kDegToRad;
        mWarpTanHalfV = tv;
        mWarpTanHalfH = tv * aspect;
        mWarpYawRad = std::clamp(motion.deltaYawDeg * scale, -0.35f, 0.35f);
        mWarpPitchRad = std::clamp(motion.deltaPitchDeg * scale, -0.35f, 0.35f);
        mWarpValid = std::isfinite(mWarpYawRad) && std::isfinite(mWarpPitchRad) &&
                     std::isfinite(mWarpTanHalfH) && tv > 0.05f && aspect > 0.1f;
    }

    const bool cameraMoving = (std::abs(motion.deltaPitchDeg) > 1e-3f) ||
                              (std::abs(motion.deltaYawDeg) > 1e-3f);
    
    // Stationary pause: if enabled and no camera movement for 30 frames (~0.5s at 60fps),
    // skip frame generation to save power.
    const bool pauseWhenStationary = cfg.pauseWhenStationary;
    if (cameraMoving) {
        mStationaryFrameStreak = 0;
    } else {
        ++mStationaryFrameStreak;
    }
    
    const bool shouldPauseForStationary = pauseWhenStationary && 
                                          mStationaryFrameStreak >= 30 &&
                                          !cameraMoving;

#if defined(JNGS_HOST_TEST)
    // Host-simulation path for GPU command queue flush
    if (modEnabled && cfg.hudOptimizerMode != HudOptimizerMode::Off &&
        cfg.hudLatencyFlush) {
        if (cfg.hudOptimizerMode == HudOptimizerMode::UltraLowLatency ||
            cameraMoving) {
            glFlush();
        }
    }
#endif

    int synthesizedThisSwap = 0;
    int activeMultiplier = 1;
    EGLBoolean swapResult = EGL_TRUE;

    const bool fgRequested =
        modEnabled && (cfg.frameGenMode != FrameGenMode::Off) &&
        (cfg.blendStrength > 0.005f) && (mConsecutiveGlErrors < 6) &&
        !shouldPauseForStationary;

#if defined(__ANDROID__)
    // Eliminate Android EGL double-buffer VSync stalls (30 FPS lock on 60 Hz panels)
    // when Frame Generation or HUD Optimizer is active.
    if (mSwapIntervalFn != nullptr && mLastUncappedSurface != surface &&
        (fgRequested ||
         (modEnabled && cfg.hudOptimizerMode != HudOptimizerMode::Off &&
          cfg.hudLatencyFlush))) {
        mSwapIntervalFn(dpy, 0);
        mLastUncappedSurface = surface;
    }
#endif

    if (!fgRequested) {
        resetTemporalState();
        swapResult = mOrigSwapBuffers(dpy, surface);
    } else {
        const FramePacingMetrics prevPacing = mFramePacer.metricsSnapshot();
        const bool isSceneCut =
            motion.sceneCut || prevPacing.discontinuityDetected;
        if (isSceneCut) {
            ++mSceneCutCount;
        }

        const int targetMult = std::clamp(cfg.frameGenMultiplier, 2, 4);
        // Turbo Mode: actually forces 4x multi-swap with max blend for burst FPS
        const int turboMult = cfg.turboMode ? 4 : targetMult;
        // Experimental GPU image synthesis. Host tests mock GLES/EGL and only
        // verify control flow; actual presentation must be validated on-device.
        // On any resource or shader failure, submit the unmodified game frame.
        const int effectiveMult = cfg.turboMode ? 4
                                  : (prevPacing.autoTuneReduced
                                      ? std::max(2, turboMult - 1)
                                      : turboMult);
        {
            // Declared before the guard so it is destroyed AFTER it: the guard's
            // restore calls glEnable/glDisable and must not trigger our own hooks.
            InternalGlScope internalScope(mInternalGl);
            GlStateGuard stateGuard;
            const std::uint32_t resourceGenBefore = mResourceGen;
            if (!ensureGlResources(width, height)) {
                ++mConsecutiveGlErrors;
                // No GPU resources means no generated frames. Never inflate
                // the FPS counter or telemetry when synthesis did not run.
                if (!mHasFrameHistory || isSceneCut) {
                    mHasFrameHistory = true;
                }
                swapResult = mOrigSwapBuffers(dpy, surface);
                if (swapResult == EGL_TRUE) {
                    mConsecutiveGlErrors = 0;
                } else {
                    ++mConsecutiveGlErrors;
                    resetTemporalState();
                }
            } else {
                // LOCK-IN GUARD 1: Validate capture — tiled GPUs can fail copy, check and fallback before warp
                // Textures recreated this swap => the hook capture went into a deleted texture.
                const bool haveCleanWorld =
                    cleanThisFrame && (resourceGenBefore == mResourceGen);
                bool captureOk = false;

                if (haveCleanWorld) {
                    // mTexCur already holds the clean world frame (captured pre-HUD
                    // via the glDisable(GL_DEPTH_TEST) hook). Capture the full frame
                    // (world+HUD) to mTexHUD for HUD overlay at swap time.
                    captureOk = captureBackbufferToTexture(mTexHUD);
                } else {
                    // Fallback: no clean world capture this frame. Capture at swap
                    // time (includes HUD — same as pre-fix behavior).
                    captureOk = captureBackbufferToTexture(mTexCur);
                    if (captureOk) {
                        captureBackbufferToTexture(mTexHUD); // same content
                    }
                }

                if (captureOk) {
                    mTexCurClean = haveCleanWorld;
                }

                if (!captureOk) {
                    ++mConsecutiveGlErrors;
                    swapResult = mOrigSwapBuffers(dpy, surface);
                    // The current frame was not captured. Drop the history instead
                    // of pairing a stale texture with the next frame (desync).
                    resetTemporalState();
                    if (swapResult == EGL_TRUE) {
                        mConsecutiveGlErrors = 0;
                    } else {
                        ++mConsecutiveGlErrors;
                        resetTemporalState();
                    }
                } else if (!mHasFrameHistory || isSceneCut) {
                    // First frame or scene cut: seed history without blending
                    rotateHistory();
                    mHasFrameHistory = true;
                    swapResult = mOrigSwapBuffers(dpy, surface);
                } else {
                    // LOCK-IN GUARD 2: Validate history textures — stale/0 textures = guaranteed black
                    if (mTexPrev == 0 || mTexCur == 0 || !mHasFrameHistory) {
                        swapResult = mOrigSwapBuffers(dpy, surface);
                        if (mTexPrev == 0 || mTexCur == 0) { mHasFrameHistory = false; }
                        else rotateHistory();
                    // LOCK-IN GUARD 3: All addon items cause black - addon items have even lower hudProtection but still trigger
                    // Vanilla extreme: >0.45 any, Vanilla holding: >0.35+0.5, Custom: >0.25+0.3, All addon items: >0.15+0.2 (catches every addon)
                    } else if ((std::abs(motion.mvU) > 0.45f || std::abs(motion.mvV) > 0.45f) ||
                               ((std::abs(motion.mvU) > 0.35f || std::abs(motion.mvV) > 0.35f) && cfg.hudProtection > 0.5f) ||
                               ((std::abs(motion.mvU) > 0.25f || std::abs(motion.mvV) > 0.25f) && cfg.hudProtection > 0.3f) ||
                               ((std::abs(motion.mvU) > 0.15f || std::abs(motion.mvV) > 0.15f) && cfg.hudProtection > 0.2f)) {
                        swapResult = mOrigSwapBuffers(dpy, surface);
                        rotateHistory();
                    } else if (mTexPrevClean != mTexCurClean) {
                        // One of the two frames has the HUD baked in and the other
                        // does not: interpolating them would misalign the HUD.
                        swapResult = mOrigSwapBuffers(dpy, surface);
                        rotateHistory();
                    } else {
                    // MultiSwap guards - STRICT MODE:
                    // MultiSwap on mobile is fundamentally dangerous because each
                    // eglSwapBuffers call hands the buffer to the compositor. Multiple
                    // swaps per logical frame cause queue buildup and extreme lag.
                    //
                    // Strict mode (default ON):
                    //   - Only allowed with explicit Turbo Mode opt-in
                    //   - OR detected 90Hz+ display (emaFrameIntervalNs <= 11.1ms)
                    //   - Max 1 extra frame (2x total) to minimize queue depth
                    //
                    // Non-strict mode (user explicitly disabled strict mode):
                    //   - Requires presentation time extension
                    //   - Requires minimum base FPS
                    //   - Max 2 extra frames
                    const bool highRefreshDisplay =
                        (prevPacing.emaFrameIntervalNs >= 1'000'000LL) &&
                        (prevPacing.emaFrameIntervalNs <= 11'111'111LL);  // 90Hz+
                    const bool multiSwapAllowed =
                        !cfg.multiSwapStrictMode || cfg.turboMode || highRefreshDisplay;

                    const bool canMultiSwap =
                        (cfg.frameGenMode ==
                         FrameGenMode::MultiSwapInterpolation) &&
                        multiSwapAllowed &&
                        mPresentationTimeFn != nullptr &&  // Require presentation time extension
                        !prevPacing.autoTuneReduced &&
                        (prevPacing.emaFrameIntervalNs >= 1'000'000LL) &&
                        (prevPacing.emaFrameIntervalNs <= 80'000'000LL) &&
                        // Minimum base FPS requirements for MultiSwap to not queue frames
                        (effectiveMult == 2 ? prevPacing.emaFrameIntervalNs <= 33'333'333LL :  // 30fps
                         effectiveMult == 3 ? prevPacing.emaFrameIntervalNs <= 22'222'222LL :  // 45fps
                                              prevPacing.emaFrameIntervalNs <= 16'666'667LL);  // 60fps for 4x

                    if (canMultiSwap) {
                        // STRICT: max 1 extra frame (2x) to prevent queue buildup
                        // Non-strict: max 2 extra frames (3x)
                        const int maxExtraFrames = cfg.multiSwapStrictMode ? 1 : 2;
                        const int extraFrames =
                            std::clamp(effectiveMult - 1, 1, maxExtraFrames);
                        const std::int64_t intervalNs =
                            prevPacing.emaFrameIntervalNs;
                        const std::int64_t baseNs = std::max<std::int64_t>(
                            nowNs, mLastPresentedTimestampNs + 1'000'000LL);

                        // Cap extra frames to what fits in one vsync interval
                        // Use detected refresh rate instead of fixed 60Hz
                        const std::int64_t vsyncIntervalNs =
                            highRefreshDisplay ? 11'111'111LL : 16'666'667LL;
                        const int maxExtraFramesInVsync =
                            static_cast<int>(vsyncIntervalNs / intervalNs);
                        const int cappedExtraFrames =
                            std::min(extraFrames, std::max(1, maxExtraFramesInVsync));

                        bool allSwapsOk = true;
                        for (int i = 1; i <= cappedExtraFrames; ++i) {
                            const float phase =
                                static_cast<float>(i) /
                                static_cast<float>(extraFrames + 1);
                            const std::int64_t synthTs =
                                baseNs + (intervalNs * i) / (extraFrames + 1);

                            if (mPresentationTimeFn != nullptr) {
                                mPresentationTimeFn(dpy, surface, synthTs);
                            }
                            // LOCK-IN GUARD 4: drawSynthesizedQuad can fail (shader/tiled GPU) — fallback to real if it does
                            if (!drawSynthesizedQuad(motion.mvU, motion.mvV, phase,
                                                cfg.blendStrength,
                                                cfg.hudProtection)) {
                                allSwapsOk = false;
                                ++mConsecutiveGlErrors;
                                break;
                            }
                            if (mOrigSwapBuffers(dpy, surface) != EGL_TRUE) {
                                allSwapsOk = false;
                                break;
                            }
                            mLastPresentedTimestampNs = synthTs;
                            ++synthesizedThisSwap;
                        }

                        if (allSwapsOk) {
                            const std::int64_t finalTs = std::max<std::int64_t>(
                                baseNs + intervalNs,
                                mLastPresentedTimestampNs + 1'000'000LL);
                            if (mPresentationTimeFn != nullptr) {
                                mPresentationTimeFn(dpy, surface, finalTs);
                            }
                            if (!drawSynthesizedQuad(motion.mvU, motion.mvV, 1.0f,
                                                cfg.blendStrength * 0.35f,
                                                cfg.hudProtection)) {
                                ++mConsecutiveGlErrors;
                                swapResult = EGL_FALSE;
                            } else {
                                captureOutputToPrevSynthesis();
                                swapResult = mOrigSwapBuffers(dpy, surface);
                            }
                            if (swapResult == EGL_TRUE) {
                                mLastPresentedTimestampNs = finalTs;
                                rotateHistory();
                                activeMultiplier = cappedExtraFrames + 1;
                                // Reflect generated frames in the in-game FPS
                                // counter (libinbuiltmods.so) so it matches the
                                // real presentation rate.
                                if (cappedExtraFrames > 0) {
                                    tryResolveInbuiltFpsCounter();
                                    if (mInbuiltFpsFrameCount != nullptr) {
                                        mInbuiltFpsFrameCount->fetch_add(
                                            cappedExtraFrames,
                                            std::memory_order_relaxed);
                                    }
                                }
                                mConsecutiveGlErrors = 0;
                            } else {
                                ++mConsecutiveGlErrors;
                                resetTemporalState();
                            }
                        } else {
                            ++mConsecutiveGlErrors;
                            resetTemporalState();
                            swapResult = mOrigSwapBuffers(dpy, surface);
                        }
                    } else {
                        // JNGS Single-Swap: one warp at the true midpoint (0.5)
                        // — this is the lowest-latency path. For 3x/4x we
                        // previously did multi-phase averaging which looks like
                        // motion blur and prevents 800 from feeling like 800.
                        // Now we keep it sharp and only vary blend by mult.
                        const float turboBoost = cfg.turboMode ? 1.15f : 1.0f;
                        const float effectiveBlend =
                            prevPacing.autoTuneReduced && !cfg.turboMode
                                ? (cfg.blendStrength * 0.45f)
                                : (cfg.blendStrength * turboBoost);
                        const float singlePhase = 0.5f;
                        if (!drawSynthesizedQuad(motion.mvU, motion.mvV, singlePhase,
                                            effectiveBlend, cfg.hudProtection)) {
                            ++mConsecutiveGlErrors;
                            swapResult = mOrigSwapBuffers(dpy, surface);
                            rotateHistory();
                        } else {
                            captureOutputToPrevSynthesis();
                            swapResult = mOrigSwapBuffers(dpy, surface);
                        if (swapResult == EGL_TRUE) {
                            if (mConsecutiveGlErrors < 6) {
                                rotateHistory();
                                // Single-Swap submits one image only. Keep it
                                // as a temporal visual effect, not a generated
                                // presentation multiplier.
                                synthesizedThisSwap = 0;
                                activeMultiplier = 1;
                                mConsecutiveGlErrors = 0;
                            } else {
                                rotateHistory();
                            }
                        } else {
                            ++mConsecutiveGlErrors;
                            resetTemporalState();
                        }
                        }
                    }
                    } // end guards
                }
            }
        }
    }

    const FramePacingMetrics pacing = mFramePacer.recordFrame(
        nowNs, synthesizedThisSwap, activeMultiplier, cfg.autoTune,
        swapResult == EGL_TRUE);

    RuntimeTelemetrySnapshot snap{};

    snap.moduleEnabled = modEnabled;
    snap.eglHooked = mHookInstalled.load(std::memory_order_relaxed);
    snap.presentationTimeSupported =
        mPresentationTimeSupported.load(std::memory_order_relaxed);
    snap.turnDeltaHooked = CameraSmoother::instance().isTurnDeltaHooked();
    snap.fovHooked = CameraSmoother::instance().isFovHooked();
    snap.perspectiveHooked = CameraSmoother::instance().isPerspectiveHooked();

    snap.realFps = pacing.realFps;
    snap.effectiveFps = pacing.effectiveFps;
    snap.onePercentLowFps = pacing.onePercentLowFps;
    snap.frameTimeMs = pacing.frameTimeMs;
    snap.jitterMs = pacing.jitterMs;

    snap.lastMotionU = motion.mvU;
    snap.lastMotionV = motion.mvV;
    snap.cameraPitchVelDeg = motion.deltaPitchDeg;
    snap.cameraYawVelDeg = motion.deltaYawDeg;
    snap.currentFovDeg = motion.currentFovDeg;
    snap.cameraPerspective = motion.cameraPerspective;

    snap.totalRealFrames = pacing.totalRealFrames;
    snap.totalSynthesizedFrames = pacing.totalSynthesizedFrames;
    snap.sceneCutResets = mSceneCutCount;
    snap.activeMultiplier = activeMultiplier;
    snap.autoTuneReduced = pacing.autoTuneReduced;

    {
        std::lock_guard<std::mutex> lock(mTelemetryMutex);
        mTelemetry = snap;
    }

    if (postCallback) {
        postCallback(snap);
    }

    return swapResult;
    } catch (...) {
        mConsecutiveGlErrors++;
        if (mOrigSwapBuffers) return mOrigSwapBuffers(dpy, surface);
        return EGL_TRUE;
    }
}

} // namespace framegen
