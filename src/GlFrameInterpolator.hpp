#pragma once

#include "CameraSmoother.hpp"
#include "FrameGenConfig.hpp"
#include "FramePacer.hpp"

#include <atomic>
#include <cstdint>
#include <functional>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <pl/memory/Hook.hpp>

namespace framegen {

class GlStateGuard {
public:
    GlStateGuard() noexcept;
    ~GlStateGuard() noexcept;

    GlStateGuard(const GlStateGuard &) = delete;
    GlStateGuard &operator=(const GlStateGuard &) = delete;

private:
    GLint mProgram = 0;
    GLint mActiveTexture = GL_TEXTURE0;
    GLint mViewport[4]{};
    GLint mScissorBox[4]{};
    GLint mFramebuffer = 0;
    GLint mReadFramebuffer = 0;
    GLint mDrawFramebuffer = 0;
    GLint mArrayBuffer = 0;
    GLint mElementArrayBuffer = 0;
    GLint mVertexArray = 0;
    GLint mTex0 = 0;
    GLint mTex1 = 0;
    GLint mTex2 = 0;
    GLint mTex3 = 0;
    GLint mBlendSrcRgb = GL_ONE;
    GLint mBlendDstRgb = GL_ZERO;
    GLint mBlendSrcAlpha = GL_ONE;
    GLint mBlendDstAlpha = GL_ZERO;

    GLboolean mDepthTest = GL_FALSE;
    GLboolean mBlend = GL_FALSE;
    GLboolean mCullFace = GL_FALSE;
    GLboolean mScissorTest = GL_FALSE;
    GLboolean mStencilTest = GL_FALSE;
    GLboolean mColorMask[4]{GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
};

class GlFrameInterpolator {
public:
    using EglSwapBuffersFn = EGLBoolean(EGLAPIENTRY *)(EGLDisplay, EGLSurface);
    using EglSwapIntervalFn = EGLBoolean(EGLAPIENTRY *)(EGLDisplay, EGLint);
    using EglPresentationTimeFn =
        EGLBoolean(EGLAPIENTRY *)(EGLDisplay, EGLSurface, std::int64_t);
    using PostFrameCallback =
        std::function<void(const RuntimeTelemetrySnapshot &)>;

    static GlFrameInterpolator &instance() noexcept;

    bool installHook() noexcept;
    void uninstallHook() noexcept;

    void setModuleEnabled(bool enabled) noexcept;
    [[nodiscard]] bool isModuleEnabled() const noexcept {
        return mModuleEnabled.load(std::memory_order_relaxed);
    }

    void updateConfig(const FrameGenConfig &config) noexcept;
    [[nodiscard]] FrameGenConfig currentConfig() const noexcept;

    void requestTemporalReset() noexcept {
        mResetRequested.store(true, std::memory_order_relaxed);
    }

    void setPostFrameCallback(PostFrameCallback callback) noexcept;

    [[nodiscard]] RuntimeTelemetrySnapshot telemetrySnapshot() const noexcept;

    // Main swap handler invoked by the eglSwapBuffers detour (and directly by host tests)
    EGLBoolean onSwapBuffers(EGLDisplay dpy, EGLSurface surface) noexcept;

    static EGLBoolean EGLAPIENTRY swapBuffersDetour(
        EGLDisplay dpy, EGLSurface surface) noexcept;

    // Test hooks for host simulation without a physical Android GPU
    void setSwapCallbacksForTest(
        EglSwapBuffersFn origSwap,
        EglPresentationTimeFn presentationTimeFn = nullptr) noexcept;
    void setInbuiltFpsCounterForTest(
        std::atomic<int> *frameCountPtr,
        std::atomic<int> *currentFpsPtr = nullptr) noexcept;
    void tryResolveInbuiltFpsCounter() noexcept;
    [[nodiscard]] static std::atomic<int> *decodeArm64InbuiltFpsFrameCount(
        const void *nativeGetFpsAddr) noexcept;

private:
    GlFrameInterpolator() = default;

public:
    // --- Helper functions (public for GlStateGuard access) ---
    static GLuint compileGlShader(GLenum type, const char *source) noexcept;
    static GLuint createTexture2D(GLsizei width, GLsizei height) noexcept;
    inline void collectGlDiagnostics(RuntimeTelemetrySnapshot &out) noexcept;
    static void clearGlErrors() noexcept;

public:
    // --- World-before-HUD capture hooks (GL level) ---
    // Called from GL detour functions (glClearDetour, glDisableDetour, glEnableDetour)
    void onFrameStart() noexcept;
    void onDepthTestDisabled() noexcept;
    void onDepthTestEnabled() noexcept;

private:
    bool installGlHooks() noexcept;
    void uninstallGlHooks() noexcept;
    bool captureWorldFrame() noexcept;
    bool captureBackbufferToTexture(GLuint targetTex) noexcept;
    void resetTemporalState() noexcept;
    void rotateHistory() noexcept;
    void captureOutputToPrevSynthesis() noexcept;
    bool drawSynthesizedQuad(float mvU, float mvV, float phase,
                             float blendWeight, float hudProtection) noexcept;

    mutable std::mutex mConfigMutex;
    FrameGenConfig mConfig{};
    PostFrameCallback mPostFrameCallback{};

    mutable std::mutex mTelemetryMutex;
    RuntimeTelemetrySnapshot mTelemetry{};

    FramePacer mFramePacer{};

    std::atomic<bool> mModuleEnabled{true};
    std::atomic<bool> mHookInstalled{false};
    std::atomic<bool> mPresentationTimeSupported{false};
    std::atomic<bool> mResetRequested{false};

    pl::memory::HookHandle mSwapHook;
    EglSwapBuffersFn mOrigSwapBuffers = nullptr;
    EglSwapIntervalFn mSwapIntervalFn = nullptr;
    EglPresentationTimeFn mPresentationTimeFn = nullptr;
    EGLSurface mLastUncappedSurface = EGL_NO_SURFACE;

    // OpenGL ES state (owned by the render thread)
    bool mGlInitialized = false;
    bool mHasFrameHistory = false;
    GLsizei mSurfaceWidth = 0;
    GLsizei mSurfaceHeight = 0;
    GLsizei mSynthWidth = 0;
    GLsizei mSynthHeight = 0;

    bool ensureGlResources(GLsizei width, GLsizei height) noexcept;
    void destroyGlResources() noexcept;
    bool compileSynthesisProgram() noexcept;

    GLuint mTexPrev = 0;
    GLuint mTexCur = 0;
    GLuint mTexHUD = 0;  // Full frame at swap time (world + HUD) for HUD overlay
    std::uint32_t mResourceGen = 0;  // bumped whenever GL textures are recreated
    bool mTexPrevClean = false;  // true when mTexPrev holds a HUD-free world capture
    bool mTexCurClean = false;   // same for mTexCur
    // Camera-rotation warp parameters for the current frame pair.
    bool mWarpValid = false;
    float mWarpYawRad = 0.0f;
    float mWarpPitchRad = 0.0f;
    float mWarpTanHalfH = 1.0f;
    float mWarpTanHalfV = 1.0f;
    GLuint mTexPrevSynthesis = 0;  // Previous frame's synthesis output for temporal smoothing
    GLuint mDownscaleFbo = 0;  // FBO for downscaling full-res backbuffer to synthesis textures
    GLuint mSynthesisProgram = 0;
    GLuint mQuadVbo = 0;

    GLint mAttrPos = -1;
    GLint mAttrUV = -1;
    GLint mUniPrevWorld = -1;   // Clean world frame from previous frame
    GLint mUniCurWorld = -1;    // Clean world frame from current frame
    GLint mUniCurFull = -1;     // Full frame (world + HUD) from current frame
    GLint mUniPrevSynthesis = -1; // Previous synthesis output for temporal smoothing
    GLint mUniMotionVec = -1;
    GLint mUniPrevRot = -1;     // cos/sin yaw+pitch for prev-frame lookup
    GLint mUniCurRot = -1;      // cos/sin yaw+pitch for cur-frame lookup
    GLint mUniTanHalf = -1;     // tan(fov/2) horizontal, vertical
    GLint mUniUseRot = -1;
    GLint mUniPhase = -1;
    GLint mUniBlendWeight = -1;
    GLint mUniHudProtection = -1;
    GLint mUniMotionBlur = -1;
    GLint mUniCrystalSharp = -1;
    GLint mUniDebugTint = -1;
    GLint mUniMotionBlurStrength = -1;  // New: proper motion blur strength
    GLint mUniBlendFalloff = -1;        // New: edge feathering
    GLint mUniHudDetectionSensitivity = -1; // New: HUD detection sensitivity
    GLint mUniTemporalSmoothing = -1;   // New: temporal smoothing
    bool mMotionBlurEnabled = false;
    bool mCrystalSharpEnabled = true;
    bool mTurboModeEnabled = false;
    bool mReflexModeEnabled = false;
    bool mDebugShowGeneratedFrames = false;

    // GL hook function pointers for world-before-HUD capture
    // Use standard function pointers (GL_APIENTRY is Android-only)
    using GlClearFn = void (*)(GLbitfield);
    using GlDisableFn = void (*)(GLenum);
    using GlEnableFn = void (*)(GLenum);

public:
    // Accessed from GL detour functions (glClearDetour, glDisableDetour, glEnableDetour)
    GlClearFn mGlClearFn = nullptr;
    GlDisableFn mGlDisableFn = nullptr;
    GlEnableFn mGlEnableFn = nullptr;

    void (*mOrigGlClear)(GLbitfield) = nullptr;
    void (*mOrigGlDisable)(GLenum) = nullptr;
    void (*mOrigGlEnable)(GLenum) = nullptr;

    // True while onSwapBuffers is issuing its own GL calls, so the GL detours ignore them.
    std::atomic<bool> mInternalGl{false};

    pl::memory::HookHandle mClearHook;
    pl::memory::HookHandle mDisableHook;
    pl::memory::HookHandle mEnableHook;

private:
    std::atomic<bool> mWorldCapturedThisFrame{false};
    std::atomic<bool> mDepthTestWasEnabled{true};
    std::atomic<bool> mDepthTestEnabled{true};

    std::int64_t mLastPresentedTimestampNs = 0;
    std::uint64_t mSceneCutCount = 0;
    std::uint32_t mConsecutiveGlErrors = 0;
    std::uint32_t mStationaryFrameStreak = 0;
    RuntimeTelemetrySnapshot mGlDiagnostics{};
    bool mGlDiagnosticsCollected = false;
    EGLContext mDiagnosticContext = EGL_NO_CONTEXT;

    std::atomic<int> *mInbuiltFpsFrameCount = nullptr;
    std::atomic<int> *mInbuiltCurrentFps = nullptr;
    std::uint32_t mInbuiltFpsResolveCooldown = 0;
};

} // namespace framegen