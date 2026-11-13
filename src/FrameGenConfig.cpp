#include "FrameGenConfig.hpp"
#include "ConfigManager.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#if !defined(__ANDROID__)
#include <fstream>
#endif
#if defined(__ANDROID__)
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#endif
#include <sstream>
#include <unordered_map>

#include <pl/ModMenuConfig.hpp>

namespace framegen {
namespace {

bool parseBoolValue(std::string_view text, bool fallback) noexcept {
    if (text == "true" || text == "1" || text == "on" || text == "yes" ||
        text == "enabled" || text == "TRUE" || text == "ON") {
        return true;
    }
    if (text == "false" || text == "0" || text == "off" || text == "no" ||
        text == "disabled" || text == "FALSE" || text == "OFF") {
        return false;
    }
    return fallback;
}

int parseIntValue(std::string_view text, int fallback) noexcept {
    if (text.empty()) {
        return fallback;
    }
    std::string buf(text);
    char *end = nullptr;
    errno = 0;
    const long val = std::strtol(buf.c_str(), &end, 10);
    if (end == buf.c_str() || errno == ERANGE) {
        return fallback;
    }
    return static_cast<int>(val);
}

float parseFloatValue(std::string_view text, float fallback) noexcept {
    if (text.empty()) {
        return fallback;
    }
    std::string buf(text);
    char *end = nullptr;
    errno = 0;
    const float val = std::strtof(buf.c_str(), &end);
    if (end == buf.c_str() || errno == ERANGE || !std::isfinite(val)) {
        return fallback;
    }
    return val;
}

std::string formatFloat(float value, int decimals = 2) {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%.*f", decimals,
                  static_cast<double>(value));
    return std::string(buf);
}

int parseMultiplierValue(std::string_view text, int fallback) noexcept {
    if (text == "2" || text == "2x" || text == "2X" || text == "2×") {
        return 2;
    }
    if (text == "3" || text == "3x" || text == "3X" || text == "3×") {
        return 3;
    }
    if (text == "4" || text == "4x" || text == "4X" || text == "4×") {
        return 4;
    }
    return std::clamp(parseIntValue(text, fallback), 2, 4);
}

QualityPreset parsePresetValue(std::string_view text,
                               QualityPreset fallback) noexcept {
    if (text == "Power Saver" || text == "power_saver") {
        return QualityPreset::PowerSaver;
    }
    if (text == "Balanced" || text == "balanced") {
        return QualityPreset::Balanced;
    }
    if (text == "Smooth" || text == "smooth") {
        return QualityPreset::Smooth;
    }
    if (text == "Ultra" || text == "ultra") {
        return QualityPreset::Ultra;
    }
    if (text == "Performance" || text == "performance") {
        return QualityPreset::Performance;
    }
    if (text == "Custom" || text == "custom") {
        return QualityPreset::Custom;
    }
    const int idx = parseIntValue(text, static_cast<int>(fallback));
    return static_cast<QualityPreset>(std::clamp(idx, 0, 5));
}

FrameGenMode parseFrameGenModeValue(std::string_view text,
                                    FrameGenMode fallback) noexcept {
    if (text == "Off" || text == "off") {
        return FrameGenMode::Off;
    }
    if (text == "Single-Swap Motion Synthesis" || text == "single_swap" ||
        text == "Single-Swap") {
        return FrameGenMode::SingleSwapSynthesis;
    }
    if (text == "Multi-Swap Frame Interpolation" || text == "multi_swap" ||
        text == "Multi-Swap") {
        return FrameGenMode::MultiSwapInterpolation;
    }
    const int idx = parseIntValue(text, static_cast<int>(fallback));
    return static_cast<FrameGenMode>(std::clamp(idx, 0, 2));
}

CameraSmoothingMode parseCameraModeValue(
    std::string_view text, CameraSmoothingMode fallback) noexcept {
    if (text == "Off" || text == "off") {
        return CameraSmoothingMode::Off;
    }
    if (text == "Exponential (EMA)" || text == "ema" || text == "Exponential") {
        return CameraSmoothingMode::Exponential;
    }
    if (text == "Critically Damped Spring" || text == "spring" ||
        text == "Spring") {
        return CameraSmoothingMode::CriticallyDampedSpring;
    }
    if (text == "Adaptive Smart (1-Euro)" || text == "adaptive" ||
        text == "Adaptive Smart") {
        return CameraSmoothingMode::AdaptiveSmart;
    }
    const int idx = parseIntValue(text, static_cast<int>(fallback));
    return static_cast<CameraSmoothingMode>(std::clamp(idx, 0, 3));
}

HudStyle parseHudStyleValue(std::string_view text, HudStyle fallback) noexcept {
    if (text == "Compact" || text == "compact") {
        return HudStyle::Compact;
    }
    if (text == "Detailed" || text == "detailed") {
        return HudStyle::Detailed;
    }
    if (text == "Minimal" || text == "minimal") {
        return HudStyle::Minimal;
    }
    const int idx = parseIntValue(text, static_cast<int>(fallback));
    return static_cast<HudStyle>(std::clamp(idx, 0, 2));
}

HudOptimizerMode parseHudOptimizerModeValue(
    std::string_view text, HudOptimizerMode fallback) noexcept {
    if (text == "Off" || text == "off") {
        return HudOptimizerMode::Off;
    }
    if (text == "Balanced" || text == "balanced") {
        return HudOptimizerMode::Balanced;
    }
    if (text == "Ultra Low-Latency" || text == "ultra" ||
        text == "Ultra Low-Latency (Recommended)") {
        return HudOptimizerMode::UltraLowLatency;
    }
    const int idx = parseIntValue(text, static_cast<int>(fallback));
    return static_cast<HudOptimizerMode>(std::clamp(idx, 0, 2));
}

// Lightweight flat JSON object parser for config.json persistence
bool parseFlatJsonObject(
    std::string_view json,
    std::unordered_map<std::string, std::string> &outMap) {
    std::size_t pos = 0;
    const std::size_t n = json.size();
    auto skipWs = [&]() {
        while (pos < n &&
               std::isspace(static_cast<unsigned char>(json[pos]))) {
            ++pos;
        }
    };

    skipWs();
    if (pos >= n || json[pos] != '{') {
        return false;
    }
    ++pos;

    while (pos < n) {
        skipWs();
        if (pos < n && json[pos] == '}') {
            ++pos;
            return true;
        }
        if (pos >= n || json[pos] != '"') {
            return false;
        }
        ++pos;
        std::string key;
        while (pos < n && json[pos] != '"') {
            if (json[pos] == '\\' && pos + 1 < n) {
                ++pos;
            }
            key.push_back(json[pos++]);
        }
        if (pos >= n || json[pos] != '"') {
            return false;
        }
        ++pos;

        skipWs();
        if (pos >= n || json[pos] != ':') {
            return false;
        }
        ++pos;
        skipWs();

        std::string val;
        if (pos < n && json[pos] == '"') {
            ++pos;
            while (pos < n && json[pos] != '"') {
                if (json[pos] == '\\' && pos + 1 < n) {
                    ++pos;
                }
                val.push_back(json[pos++]);
            }
            if (pos >= n || json[pos] != '"') {
                return false;
            }
            ++pos;
        } else {
            while (pos < n && json[pos] != ',' && json[pos] != '}' &&
                   !std::isspace(static_cast<unsigned char>(json[pos]))) {
                val.push_back(json[pos++]);
            }
        }

        outMap[std::move(key)] = std::move(val);
        skipWs();
        if (pos < n && json[pos] == ',') {
            ++pos;
        }
    }
    return false;
}

} // namespace

const char *presetName(QualityPreset preset) noexcept {
    switch (preset) {
    case QualityPreset::PowerSaver:
        return "Power Saver";
    case QualityPreset::Balanced:
        return "Balanced";
    case QualityPreset::Smooth:
        return "Smooth";
    case QualityPreset::Ultra:
        return "Ultra";
    case QualityPreset::Performance:
        return "Performance";
    case QualityPreset::Custom:
        return "Custom";
    }
    return "Balanced";
}

const char *frameGenModeName(FrameGenMode mode) noexcept {
    switch (mode) {
    case FrameGenMode::Off:
        return "Off";
    case FrameGenMode::SingleSwapSynthesis:
        return "Single-Swap Motion Synthesis";
    case FrameGenMode::MultiSwapInterpolation:
        return "Multi-Swap Frame Interpolation";
    }
    return "Single-Swap Motion Synthesis";
}

const char *cameraSmoothingModeName(CameraSmoothingMode mode) noexcept {
    switch (mode) {
    case CameraSmoothingMode::Off:
        return "Off";
    case CameraSmoothingMode::Exponential:
        return "Exponential (EMA)";
    case CameraSmoothingMode::CriticallyDampedSpring:
        return "Critically Damped Spring";
    case CameraSmoothingMode::AdaptiveSmart:
        return "Adaptive Smart (1-Euro)";
    }
    return "Adaptive Smart (1-Euro)";
}

const char *hudStyleName(HudStyle style) noexcept {
    switch (style) {
    case HudStyle::Compact:
        return "Compact";
    case HudStyle::Detailed:
        return "Detailed";
    case HudStyle::Minimal:
        return "Minimal";
    }
    return "Minimal";
}

const char *hudOptimizerModeName(HudOptimizerMode mode) noexcept {
    switch (mode) {
    case HudOptimizerMode::Off:
        return "Off";
    case HudOptimizerMode::Balanced:
        return "Balanced";
    case HudOptimizerMode::UltraLowLatency:
        return "Ultra Low-Latency";
    }
    return "Ultra Low-Latency";
}

void FrameGenConfig::normalize() noexcept {
    schemaVersion = 3;
    preset = static_cast<QualityPreset>(
        std::clamp(static_cast<int>(preset), 0, 4));
    frameGenMode = static_cast<FrameGenMode>(
        std::clamp(static_cast<int>(frameGenMode), 0, 2));
    frameGenMultiplier = std::clamp(frameGenMultiplier, 2, 4);
    blendStrength = std::clamp(
        std::isfinite(blendStrength) ? blendStrength : 0.65f, 0.0f, 1.0f);
    motionScale = std::clamp(
        std::isfinite(motionScale) ? motionScale : 1.0f, 0.0f, 2.0f);
    hudProtection = std::clamp(
        std::isfinite(hudProtection) ? hudProtection : 0.88f, 0.0f, 1.0f);
    sceneCutThreshold = std::clamp(
        std::isfinite(sceneCutThreshold) ? sceneCutThreshold : 42.0f, 10.0f,
        120.0f);

    cameraSmoothingMode = static_cast<CameraSmoothingMode>(
        std::clamp(static_cast<int>(cameraSmoothingMode), 0, 3));
    cameraSmoothness = std::clamp(
        std::isfinite(cameraSmoothness) ? cameraSmoothness : 0.55f, 0.0f,
        0.95f);
    cameraResponsiveness = std::clamp(
        std::isfinite(cameraResponsiveness) ? cameraResponsiveness : 1.05f,
        0.1f, 2.5f);
    microDeadzone = std::clamp(
        std::isfinite(microDeadzone) ? microDeadzone : 0.025f, 0.0f, 0.5f);
    spikeFilterDeg = std::clamp(
        std::isfinite(spikeFilterDeg) ? spikeFilterDeg : 75.0f, 10.0f, 180.0f);

    if (targetFps != 0) {
        targetFps = std::clamp(targetFps, 30, 240);
    }

    synthesisScale = std::clamp(
        std::isfinite(synthesisScale) ? synthesisScale : 1.0f, 0.25f, 1.0f);

    motionBlurStrength = std::clamp(
        std::isfinite(motionBlurStrength) ? motionBlurStrength : 0.0f, 0.0f, 1.0f);

    blendFalloff = std::clamp(
        std::isfinite(blendFalloff) ? blendFalloff : 0.15f, 0.0f, 0.5f);
    hudDetectionSensitivity = std::clamp(
        std::isfinite(hudDetectionSensitivity) ? hudDetectionSensitivity : 1.0f, 0.5f, 2.0f);
    temporalSmoothing = std::clamp(
        std::isfinite(temporalSmoothing) ? temporalSmoothing : 0.3f, 0.0f, 1.0f);

    hudOptimizerMode = static_cast<HudOptimizerMode>(
        std::clamp(static_cast<int>(hudOptimizerMode), 0, 2));

    hudStyle = static_cast<HudStyle>(
        std::clamp(static_cast<int>(hudStyle), 0, 2));
    hudOpacity = std::clamp(hudOpacity, 20, 100);
    hudPosX = std::clamp(std::isfinite(hudPosX) ? hudPosX : 16.0f, 0.0f,
                         4096.0f);
    hudPosY = std::clamp(std::isfinite(hudPosY) ? hudPosY : 16.0f, 0.0f,
                         4096.0f);
}

void FrameGenConfig::applyPreset(QualityPreset newPreset) noexcept {
    preset = newPreset;
    switch (newPreset) {
    case QualityPreset::PowerSaver:
        frameGenMode = FrameGenMode::SingleSwapSynthesis;
        frameGenMultiplier = 2;
        blendStrength = 0.45f;
        motionScale = 0.85f;
        hudProtection = 0.92f;
        sceneCutThreshold = 35.0f;
        cameraSmoothingMode = CameraSmoothingMode::Exponential;
        cameraSmoothness = 0.40f;
        cameraResponsiveness = 1.15f;
        microDeadzone = 0.02f;
        spikeFilterDeg = 80.0f;
        fovScaling = true;
        synthesisScale = 0.75f;
        autoTune = true;
        enableMotionBlur = false;
        motionBlurStrength = 0.0f;
        pauseWhenStationary = true;
        blendFalloff = 0.2f;
        hudDetectionSensitivity = 1.2f;
        temporalSmoothing = 0.4f;
        multiSwapStrictMode = true;
        break;

    case QualityPreset::Balanced:
        frameGenMode = FrameGenMode::SingleSwapSynthesis;
        frameGenMultiplier = 2;
        blendStrength = 0.65f;
        motionScale = 1.0f;
        hudProtection = 0.88f;
        sceneCutThreshold = 42.0f;
        cameraSmoothingMode = CameraSmoothingMode::AdaptiveSmart;
        cameraSmoothness = 0.55f;
        cameraResponsiveness = 1.05f;
        microDeadzone = 0.025f;
        spikeFilterDeg = 75.0f;
        fovScaling = true;
        synthesisScale = 1.0f;
        autoTune = true;
        enableMotionBlur = false;
        motionBlurStrength = 0.0f;
        pauseWhenStationary = false;
        blendFalloff = 0.15f;
        hudDetectionSensitivity = 1.0f;
        temporalSmoothing = 0.3f;
        multiSwapStrictMode = true;
        break;

    case QualityPreset::Smooth:
        frameGenMode = FrameGenMode::SingleSwapSynthesis;
        frameGenMultiplier = 3;
        blendStrength = 0.78f;
        motionScale = 1.05f;
        hudProtection = 0.86f;
        sceneCutThreshold = 48.0f;
        cameraSmoothingMode = CameraSmoothingMode::AdaptiveSmart;
        cameraSmoothness = 0.68f;
        cameraResponsiveness = 0.95f;
        microDeadzone = 0.03f;
        spikeFilterDeg = 70.0f;
        fovScaling = true;
        synthesisScale = 0.75f;
        autoTune = true;
        enableMotionBlur = true;
        motionBlurStrength = 0.3f;
        pauseWhenStationary = false;
        blendFalloff = 0.12f;
        hudDetectionSensitivity = 0.9f;
        temporalSmoothing = 0.25f;
        multiSwapStrictMode = true;
        break;

    case QualityPreset::Ultra:
        frameGenMode = FrameGenMode::MultiSwapInterpolation;
        frameGenMultiplier = 4;
        blendStrength = 0.82f;
        motionScale = 1.10f;
        hudProtection = 0.85f;
        sceneCutThreshold = 55.0f;
        cameraSmoothingMode = CameraSmoothingMode::AdaptiveSmart;
        cameraSmoothness = 0.62f;
        cameraResponsiveness = 1.15f;
        microDeadzone = 0.03f;
        spikeFilterDeg = 65.0f;
        fovScaling = true;
        synthesisScale = 1.0f;
        autoTune = true;
        enableMotionBlur = true;
        motionBlurStrength = 0.4f;
        pauseWhenStationary = false;
        blendFalloff = 0.1f;
        hudDetectionSensitivity = 0.8f;
        temporalSmoothing = 0.2f;
        multiSwapStrictMode = false; // Ultra allows MultiSwap without strict checks
        break;

    case QualityPreset::Performance:
        frameGenMode = FrameGenMode::SingleSwapSynthesis;
        frameGenMultiplier = 2;
        blendStrength = 0.55f;
        motionScale = 0.9f;
        hudProtection = 0.9f;
        sceneCutThreshold = 35.0f;
        cameraSmoothingMode = CameraSmoothingMode::Exponential;
        cameraSmoothness = 0.4f;
        cameraResponsiveness = 1.2f;
        microDeadzone = 0.03f;
        spikeFilterDeg = 80.0f;
        fovScaling = true;
        synthesisScale = 0.5f;
        autoTune = true;
        turboMode = false;
        reflexMode = false;
        crystalSharp = false;
        killVignette = true;
        enableMotionBlur = false;
        motionBlurStrength = 0.0f;
        pauseWhenStationary = true;
        blendFalloff = 0.2f;
        hudDetectionSensitivity = 1.2f;
        temporalSmoothing = 0.4f;
        multiSwapStrictMode = true;
        break;

    case QualityPreset::Custom:
        break;
    }
    normalize();
}

ConfigApplyResult applyConfigKeyValue(FrameGenConfig &config,
                                      std::string_view key,
                                      std::string_view value) noexcept {
    if (key == keys::kActionResetDefaults) {
        const float savedX = config.hudPosX;
        const float savedY = config.hudPosY;
        config = FrameGenConfig{};
        config.hudPosX = savedX;
        config.hudPosY = savedY;
        config.normalize();
        return ConfigApplyResult::TriggeredResetDefaults;
    }

    if (key == keys::kActionResetTemporal) {
        return ConfigApplyResult::TriggeredResetTemporal;
    }

    if (key == keys::kActionCyclePreset) {
        const int nextIdx = (static_cast<int>(config.preset) + 1) % 6;
        config.applyPreset(static_cast<QualityPreset>(nextIdx));
        return ConfigApplyResult::TriggeredCyclePreset;
    }

    if (key == keys::kPreset) {
        const QualityPreset p = parsePresetValue(value, config.preset);
        config.applyPreset(p);
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kFrameGenMode) {
        config.frameGenMode =
            parseFrameGenModeValue(value, config.frameGenMode);
        config.preset = QualityPreset::Custom;
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kFrameGenMultiplier) {
        config.frameGenMultiplier =
            parseMultiplierValue(value, config.frameGenMultiplier);
        config.preset = QualityPreset::Custom;
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kBlendStrength) {
        config.blendStrength = parseFloatValue(value, config.blendStrength);
        config.preset = QualityPreset::Custom;
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kMotionScale) {
        config.motionScale = parseFloatValue(value, config.motionScale);
        config.preset = QualityPreset::Custom;
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kHudProtection) {
        config.hudProtection = parseFloatValue(value, config.hudProtection);
        config.preset = QualityPreset::Custom;
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kSceneCutThreshold) {
        config.sceneCutThreshold =
            parseFloatValue(value, config.sceneCutThreshold);
        config.preset = QualityPreset::Custom;
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kSynthesisScale) {
        config.synthesisScale = parseFloatValue(value, config.synthesisScale);
        config.preset = QualityPreset::Custom;
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kBlendFalloff) {
        config.blendFalloff = parseFloatValue(value, config.blendFalloff);
        config.preset = QualityPreset::Custom;
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kHudDetectionSensitivity) {
        config.hudDetectionSensitivity = parseFloatValue(value, config.hudDetectionSensitivity);
        config.preset = QualityPreset::Custom;
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kTemporalSmoothing) {
        config.temporalSmoothing = parseFloatValue(value, config.temporalSmoothing);
        config.preset = QualityPreset::Custom;
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kCameraSmoothingMode) {
        config.cameraSmoothingMode =
            parseCameraModeValue(value, config.cameraSmoothingMode);
        config.preset = QualityPreset::Custom;
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kCameraSmoothness) {
        config.cameraSmoothness =
            parseFloatValue(value, config.cameraSmoothness);
        config.preset = QualityPreset::Custom;
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kCameraResponsiveness) {
        config.cameraResponsiveness =
            parseFloatValue(value, config.cameraResponsiveness);
        config.preset = QualityPreset::Custom;
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kMicroDeadzone) {
        config.microDeadzone = parseFloatValue(value, config.microDeadzone);
        config.preset = QualityPreset::Custom;
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kSpikeFilterDeg) {
        config.spikeFilterDeg = parseFloatValue(value, config.spikeFilterDeg);
        config.preset = QualityPreset::Custom;
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kTurboMode) {
        config.turboMode = parseBoolValue(value, config.turboMode);
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kReflexMode) {
        config.reflexMode = parseBoolValue(value, config.reflexMode);
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kCrystalSharp) {
        config.crystalSharp = parseBoolValue(value, config.crystalSharp);
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kKillVignette) {
        config.killVignette = parseBoolValue(value, config.killVignette);
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kDebugShowGeneratedFrames) {
        config.debugShowGeneratedFrames =
            parseBoolValue(value, config.debugShowGeneratedFrames);
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kMultiSwapStrictMode) {
        config.multiSwapStrictMode = parseBoolValue(value, config.multiSwapStrictMode);
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kFovScaling) {
        config.fovScaling = parseBoolValue(value, config.fovScaling);
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kAutoTune) {
        config.autoTune = parseBoolValue(value, config.autoTune);
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kTargetFps) {
        config.targetFps = parseIntValue(value, config.targetFps);
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kHudOptimizerMode) {
        config.hudOptimizerMode =
            parseHudOptimizerModeValue(value, config.hudOptimizerMode);
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kHudLatencyFlush) {
        config.hudLatencyFlush = parseBoolValue(value, config.hudLatencyFlush);
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kHudStaticScreenBoost) {
        config.hudStaticScreenBoost =
            parseBoolValue(value, config.hudStaticScreenBoost);
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kClearScreenOverlays) {
        config.clearScreenOverlays =
            parseBoolValue(value, config.clearScreenOverlays);
        if (config.clearScreenOverlays) {
            config.showHud = false;
        }
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kEnableMotionBlur) {
        config.enableMotionBlur =
            parseBoolValue(value, config.enableMotionBlur);
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kMotionBlurStrength) {
        config.motionBlurStrength = parseFloatValue(value, config.motionBlurStrength);
        config.preset = QualityPreset::Custom;
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kPauseWhenStationary) {
        config.pauseWhenStationary =
            parseBoolValue(value, config.pauseWhenStationary);
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kShowHud) {
        config.showHud = parseBoolValue(value, config.showHud);
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kHudStyle) {
        config.hudStyle = parseHudStyleValue(value, config.hudStyle);
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kHudOpacity) {
        config.hudOpacity = parseIntValue(value, config.hudOpacity);
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kHudPosX || key == "hudPosX") {
        config.hudPosX = parseFloatValue(value, config.hudPosX);
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    if (key == keys::kHudPosY || key == "hudPosY") {
        config.hudPosY = parseFloatValue(value, config.hudPosY);
        config.normalize();
        return ConfigApplyResult::UpdatedConfig;
    }

    return ConfigApplyResult::Unchanged;
}

void appendV1ModuleConfigs(pl::modmenu::ModuleBuilder &builder,
                           const FrameGenConfig &config) {
    // Cool toggles FIRST so they are actually seen at top of Frame Generation menu
    builder
        .config(keys::kTurboMode,
                "TURBO 4x Burst (forces 4x warp)",
                pl::modmenu::ConfigType::Toggle,
                config.turboMode ? "true" : "false")
        .config(keys::kReflexMode,
                "Reflex Low Latency (instant present)",
                pl::modmenu::ConfigType::Toggle,
                config.reflexMode ? "true" : "false")
        .config(keys::kCrystalSharp,
                "Crystal Sharp (extra sharp, zero ghost)",
                pl::modmenu::ConfigType::Toggle,
                config.crystalSharp ? "true" : "false")
        .config(keys::kKillVignette,
                "Vignette Killer (kills dark overlay)",
                pl::modmenu::ConfigType::Toggle,
                config.killVignette ? "true" : "false")
        .config(keys::kDebugShowGeneratedFrames,
                "Debug: Tint Generated Frames Red",
                pl::modmenu::ConfigType::Toggle,
                config.debugShowGeneratedFrames ? "true" : "false")
        .config(keys::kPreset,
                "Preset (0=PowerSaver, 1=Balanced, 2=Smooth, 3=Ultra, 4=Performance, 5=Custom)",
                pl::modmenu::ConfigType::SliderInt,
                std::to_string(static_cast<int>(config.preset)), "0", "5")
        .config(keys::kFrameGenMultiplier,
                "Rendered Frames Multiplier (2x / 3x / 4x Max)",
                pl::modmenu::ConfigType::SliderInt,
                std::to_string(config.frameGenMultiplier), "2", "4")
        .config(keys::kFrameGenMode,
                "FrameGen Mode (0=Off, 1=Single-Swap Visual, 2=Multi-Swap Presentation)",
                pl::modmenu::ConfigType::SliderInt,
                std::to_string(static_cast<int>(config.frameGenMode)), "0", "2")
        .config(keys::kBlendStrength,
                "Temporal Synthesis Strength",
                pl::modmenu::ConfigType::SliderFloat,
                formatFloat(config.blendStrength, 2), "0.00", "1.00")
        .config(keys::kMotionScale,
                "Optical Motion Vector Scale",
                pl::modmenu::ConfigType::SliderFloat,
                formatFloat(config.motionScale, 2), "0.00", "2.00")
        .config(keys::kHudProtection,
                "HUD / Crosshair Edge Protection",
                pl::modmenu::ConfigType::SliderFloat,
                formatFloat(config.hudProtection, 2), "0.00", "1.00")
        .config(keys::kSceneCutThreshold,
                "Scene-Cut Reset Threshold (deg/frame)",
                pl::modmenu::ConfigType::SliderFloat,
                formatFloat(config.sceneCutThreshold, 1), "10.0", "120.0")
        .config(keys::kSynthesisScale,
                "Synthesis Resolution Scale (0.25=1/4, 0.5=1/2, 0.75=3/4, 1.0=Native)",
                pl::modmenu::ConfigType::SliderFloat,
                formatFloat(config.synthesisScale, 2), "0.25", "1.00")
        .config(keys::kBlendFalloff,
                "Edge Feathering / Blend Falloff (0.00-0.50)",
                pl::modmenu::ConfigType::SliderFloat,
                formatFloat(config.blendFalloff, 2), "0.00", "0.50")
        .config(keys::kHudDetectionSensitivity,
                "HUD Detection Sensitivity (0.50-2.00)",
                pl::modmenu::ConfigType::SliderFloat,
                formatFloat(config.hudDetectionSensitivity, 2), "0.50", "2.00")
        .config(keys::kTemporalSmoothing,
                "Temporal Smoothing (0.00-1.00)",
                pl::modmenu::ConfigType::SliderFloat,
                formatFloat(config.temporalSmoothing, 2), "0.00", "1.00")
        .config(keys::kMultiSwapStrictMode,
                "Strict MultiSwap Mode (prevents 60Hz lag)",
                pl::modmenu::ConfigType::Toggle,
                config.multiSwapStrictMode ? "true" : "false")
        .config(keys::kCameraSmoothingMode,
                "Camera Smoothing (0=Off, 1=EMA, 2=Spring, 3=Adaptive Smart)",
                pl::modmenu::ConfigType::SliderInt,
                std::to_string(static_cast<int>(config.cameraSmoothingMode)),
                "0", "3")
        .config(keys::kCameraSmoothness,
                "Camera Smoothing Strength",
                pl::modmenu::ConfigType::SliderFloat,
                formatFloat(config.cameraSmoothness, 2), "0.00", "0.95")
        .config(keys::kCameraResponsiveness,
                "Camera Flick Responsiveness",
                pl::modmenu::ConfigType::SliderFloat,
                formatFloat(config.cameraResponsiveness, 2), "0.10", "2.50")
        .config(keys::kMicroDeadzone,
                "Touch Micro-Jitter Deadzone (deg)",
                pl::modmenu::ConfigType::SliderFloat,
                formatFloat(config.microDeadzone, 3), "0.000", "0.500")
        .config(keys::kSpikeFilterDeg,
                "Touch Turn-Spike Clamp (deg)",
                pl::modmenu::ConfigType::SliderFloat,
                formatFloat(config.spikeFilterDeg, 1), "10.0", "180.0")
        .config(keys::kFovScaling,
                "FOV-Aware Zoom Sensitivity Scaling",
                pl::modmenu::ConfigType::Toggle,
                config.fovScaling ? "true" : "false")
        .config(keys::kAutoTune,
                "Smart Auto-Tune Frame Budget Guard",
                pl::modmenu::ConfigType::Toggle,
                config.autoTune ? "true" : "false")
        .config(keys::kTargetFps,
                "Frame Pacer Target FPS (0=Unlimited)",
                pl::modmenu::ConfigType::SliderInt,
                std::to_string(config.targetFps), "0", "240")
        .config(keys::kHudOptimizerMode,
                "HUD Optimizer (0=Off, 1=Balanced, 2=Ultra Low-Latency)",
                pl::modmenu::ConfigType::SliderInt,
                std::to_string(static_cast<int>(config.hudOptimizerMode)), "0",
                "2")
        .config(keys::kHudLatencyFlush,
                "HUD Optimizer: Low-Latency GPU Flush",
                pl::modmenu::ConfigType::Toggle,
                config.hudLatencyFlush ? "true" : "false")
        .config(keys::kHudStaticScreenBoost,
                "HUD Optimizer: Inventory & UI Screen Boost",
                pl::modmenu::ConfigType::Toggle,
                config.hudStaticScreenBoost ? "true" : "false")
        .config(keys::kClearScreenOverlays,
                "HUD Optimizer: Remove All Screen Overlays",
                pl::modmenu::ConfigType::Toggle,
                config.clearScreenOverlays ? "true" : "false")
        .config(keys::kEnableMotionBlur,
                "Motion Blur (ON=cinematic blur, OFF=sharp no-blur)",
                pl::modmenu::ConfigType::Toggle,
                config.enableMotionBlur ? "true" : "false")
        .config(keys::kMotionBlurStrength,
                "Motion Blur Strength (0.000-1.000)",
                pl::modmenu::ConfigType::SliderFloat,
                formatFloat(config.motionBlurStrength, 3), "0.000", "1.000")
        .config(keys::kPauseWhenStationary,
                "Pause When Stationary (AFK)",
                pl::modmenu::ConfigType::Toggle,
                config.pauseWhenStationary ? "true" : "false")
        .config(keys::kShowHud,
                "Show Optional Telemetry Badge",
                pl::modmenu::ConfigType::Toggle,
                config.showHud ? "true" : "false")
        .config(keys::kHudStyle,
                "HUD Style (0=Compact, 1=Detailed, 2=Minimal)",
                pl::modmenu::ConfigType::SliderInt,
                std::to_string(static_cast<int>(config.hudStyle)), "0", "2")
        .config(keys::kHudOpacity,
                "HUD Card Opacity (%)",
                pl::modmenu::ConfigType::SliderInt,
                std::to_string(config.hudOpacity), "20", "100");
}

std::string buildV2ConfigSchemaJson(const FrameGenConfig &config,
                                    const RuntimeTelemetrySnapshot &telemetry) {
    using pl::modmenu::ConfigChoiceStyleV2;
    using pl::modmenu::ConfigConditionOpV2;
    using pl::modmenu::ConfigConditionV2;
    using pl::modmenu::ConfigControlTypeV2;
    using pl::modmenu::ConfigNodeV2;
    using pl::modmenu::ConfigOptionV2;
    using pl::modmenu::ConfigSchemaBuilder;

    char fpsInfoBuf[180]{};
    std::snprintf(fpsInfoBuf, sizeof(fpsInfoBuf),
                  "Native: %.1f FPS | Effective: %.1f FPS (%dx) | 1%% Low: %.1f | "
                  "Frame: %.2f ms (Jitter: %.2f ms)",
                  static_cast<double>(telemetry.realFps),
                  static_cast<double>(telemetry.effectiveFps),
                  telemetry.activeMultiplier,
                  static_cast<double>(telemetry.onePercentLowFps),
                  static_cast<double>(telemetry.frameTimeMs),
                  static_cast<double>(telemetry.jitterMs));

    char camInfoBuf[180]{};
    std::snprintf(camInfoBuf, sizeof(camInfoBuf),
                  "Mode: %s | FOV: %.1f deg | Turn Vel: (%.2f, %.2f) deg/f | "
                  "Motion UV: (%.4f, %.4f)",
                  cameraSmoothingModeName(config.cameraSmoothingMode),
                  static_cast<double>(telemetry.currentFovDeg),
                  static_cast<double>(telemetry.cameraYawVelDeg),
                  static_cast<double>(telemetry.cameraPitchVelDeg),
                  static_cast<double>(telemetry.lastMotionU),
                  static_cast<double>(telemetry.lastMotionV));

    char hookInfoBuf[180]{};
    std::snprintf(hookInfoBuf, sizeof(hookInfoBuf),
                  "EGL Swap: %s | TurnDelta: %s | CameraAPI FOV: %s | "
                  "Perspective: %s",
                  telemetry.eglHooked ? "Active" : "Standby",
                  telemetry.turnDeltaHooked ? "Hooked" : "Fallback",
                  telemetry.fovHooked ? "Hooked" : "Default 70",
                  telemetry.perspectiveHooked ? "Hooked" : "Auto");

    const std::vector<ConfigConditionV2> fgEnabledCond = {
        {keys::kFrameGenMode, ConfigConditionOpV2::NotEquals, "0"}
    };
    const std::vector<ConfigConditionV2> camEnabledCond = {
        {keys::kCameraSmoothingMode, ConfigConditionOpV2::NotEquals, "0"}
    };
    const std::vector<ConfigConditionV2> hudOptEnabledCond = {
        {keys::kHudOptimizerMode, ConfigConditionOpV2::NotEquals, "0"}
    };
    const std::vector<ConfigConditionV2> hudEnabledCond = {
        {keys::kShowHud, ConfigConditionOpV2::Truthy, "true"}
    };

    ConfigSchemaBuilder schema;
    // Original LeviFrame Gen toggles ONLY - just using BedrockTools WAY (worker thread + Modules JSON) not their toggles
    schema.defaultCategory("general")
        .category("general", "Presets & Multiplier",
                  "Quick profiles, 2x/3x/4x frame multiplier, and live telemetry.")
        .category("framegen", "Frame Generation",
                  "Multi-rate sub-frame synthesis and FPS multiplication.")
        .category("camera", "Camera Smoothing",
                  "Sub-frame camera smoothing with 100% angle conservation.")
        .category("pacing", "HUD Optimizer & Pacing",
                  "HUD latency optimizer, UI screen boost, and frame pacing.");

    // --- Category: general ---
    {
        ConfigNodeV2 presetNode{};
        presetNode.id = keys::kPreset;
        presetNode.key = keys::kPreset;
        presetNode.category = "general";
        presetNode.section = "Quality Profile";
        presetNode.title = "Performance & Smoothness Preset";
        presetNode.description =
            "Automatically tunes Frame Generation and Camera Smoothing.";
        presetNode.type = ConfigControlTypeV2::Choice;
        presetNode.choiceStyle = ConfigChoiceStyleV2::Segmented;
        presetNode.defaultValue = "1";
        presetNode.currentValue =
            std::to_string(static_cast<int>(config.preset));
        presetNode.hasCurrentValue = true;
        presetNode.options = {
            {"0", "Power Saver", "Minimal GPU overhead, 2x synthesis", "", false, "", false},
            {"1", "Balanced", "Recommended 2x synthesis + Smart Camera", "", false, "", false},
            {"2", "Smooth", "3x high-strength synthesis + silky camera filter", "", false, "", false},
            {"3", "Ultra", "4x multi-swap interpolation + ultra-fast response", "", false, "", false},
            {"4", "Performance", "Maximum FPS, 0.5x synthesis scale, minimal overhead", "", false, "", false},
            {"5", "Custom", "User-customized settings", "", false, "", false},
        };
        schema.node(std::move(presetNode));
    }

    {
        ConfigNodeV2 multNode{};
        multNode.id = keys::kFrameGenMultiplier;
        multNode.key = keys::kFrameGenMultiplier;
        multNode.category = "general";
        multNode.section = "Frame Multiplier (2× / 3× / 4× Max)";
        multNode.title = "Rendered Frame Multiplier";
        multNode.description =
            "Choose how many frames are generated per real game frame "
            "(2× Double, 3× Triple, or 4× Quad Max).";
        multNode.type = ConfigControlTypeV2::Choice;
        multNode.choiceStyle = ConfigChoiceStyleV2::Segmented;
        multNode.defaultValue = "2";
        multNode.currentValue = std::to_string(config.frameGenMultiplier);
        multNode.hasCurrentValue = true;
        multNode.visibleWhen = fgEnabledCond;
        multNode.enabledWhen = fgEnabledCond;
        multNode.options = {
            {"2", "2×", "Double FPS (1 generated frame per real frame)", "", false, "", false},
            {"3", "3×", "Triple FPS (2 generated frames per real frame)", "", false, "", false},
            {"4", "4×", "Quad FPS — Max (3 generated frames per real frame)", "", false, "", false},
        };
        schema.node(std::move(multNode));
    }

    auto addInfoNode = [&](const char *id, const char *title,
                           const char *desc) {
        ConfigNodeV2 n{};
        n.id = id;
        n.key = id;
        n.category = "general";
        n.section = "Live Telemetry";
        n.title = title;
        n.description = desc;
        n.type = ConfigControlTypeV2::Info;
        n.currentValue = desc;
        n.hasCurrentValue = true;
        schema.node(std::move(n));
    };
    addInfoNode("fps_telemetry", "Frame Rate & Pacing", fpsInfoBuf);
    addInfoNode("camera_telemetry", "Camera & Optical Flow", camInfoBuf);
    addInfoNode("hook_telemetry", "Native Engine Hooks", hookInfoBuf);

    auto addButtonNode = [&](const char *key, const char *title,
                             const char *desc, const char *label) {
        ConfigNodeV2 n{};
        n.id = key;
        n.key = key;
        n.category = "general";
        n.section = "Quick Actions";
        n.title = title;
        n.description = desc;
        n.type = ConfigControlTypeV2::Button;
        n.placeholder = label;
        n.actionValue = "1";
        schema.node(std::move(n));
    };
    addButtonNode(keys::kActionCyclePreset, "Cycle Quality Preset",
                  "Cycles through Power Saver, Balanced, Smooth, and Ultra.",
                  "Cycle Preset");
    addButtonNode(keys::kActionResetTemporal, "Flush Temporal History",
                  "Clears buffered frame textures and camera velocity state.",
                  "Flush History");
    addButtonNode(keys::kActionResetDefaults, "Restore Default Settings",
                  "Resets all settings back to Balanced defaults.",
                  "Reset Defaults");

    // --- Category: framegen ---
    {
        ConfigNodeV2 modeNode{};
        modeNode.id = keys::kFrameGenMode;
        modeNode.key = keys::kFrameGenMode;
        modeNode.category = "framegen";
        modeNode.section = "Synthesis Mode";
        modeNode.title = "Frame Generation Mode";
        modeNode.description =
            "Single-Swap submits one temporally blended image. Multi-Swap "
            "attempts additional intermediate EGL submissions using "
            "eglPresentationTimeANDROID; actual display scanout is platform-dependent.";
        modeNode.type = ConfigControlTypeV2::Choice;
        modeNode.choiceStyle = ConfigChoiceStyleV2::Radio;
        modeNode.defaultValue = "1";
        modeNode.currentValue =
            std::to_string(static_cast<int>(config.frameGenMode));
        modeNode.hasCurrentValue = true;
        modeNode.options = {
            {"0", "Off", "Pass-through rendering (Camera Smoothing still active)", "", false, "", false},
            {"1", "Single-Swap Visual Synthesis", "One submitted image with a temporal blend; does not multiply presentation rate", "", false, "", false},
            {"2", "Multi-Swap Frame Interpolation", "Attempts 1-3 additional EGL submissions; display presentation depends on Android compositor and refresh rate", "", false, "", false},
        };
        schema.node(std::move(modeNode));
    }

    auto addSliderInt = [&](const char *key, const char *cat, const char *sec,
                            const char *title, const char *desc, int cur,
                            int defVal, int minV, int maxV, int stepV,
                            const char *unit,
                            const std::vector<ConfigConditionV2> &vis,
                            const std::vector<ConfigConditionV2> &en) {
        ConfigNodeV2 n{};
        n.id = key;
        n.key = key;
        n.category = cat;
        n.section = sec;
        n.title = title;
        n.description = desc;
        n.type = ConfigControlTypeV2::SliderInt;
        n.defaultValue = std::to_string(defVal);
        n.currentValue = std::to_string(cur);
        n.hasCurrentValue = true;
        n.minValue = std::to_string(minV);
        n.maxValue = std::to_string(maxV);
        n.step = std::to_string(stepV);
        n.unit = unit;
        n.visibleWhen = vis;
        n.enabledWhen = en;
        schema.node(std::move(n));
    };

    auto addSliderFloat =
        [&](const char *key, const char *cat, const char *sec,
            const char *title, const char *desc, float cur, float defVal,
            float minV, float maxV, float stepV, int decimals, const char *unit,
            const std::vector<ConfigConditionV2> &vis,
            const std::vector<ConfigConditionV2> &en) {
            ConfigNodeV2 n{};
            n.id = key;
            n.key = key;
            n.category = cat;
            n.section = sec;
            n.title = title;
            n.description = desc;
            n.type = ConfigControlTypeV2::SliderFloat;
            n.defaultValue = formatFloat(defVal, decimals);
            n.currentValue = formatFloat(cur, decimals);
            n.hasCurrentValue = true;
            n.minValue = formatFloat(minV, decimals);
            n.maxValue = formatFloat(maxV, decimals);
            n.step = formatFloat(stepV, decimals);
            n.unit = unit;
            n.visibleWhen = vis;
            n.enabledWhen = en;
            schema.node(std::move(n));
        };

    auto addToggle = [&](const char *key, const char *cat, const char *sec,
                         const char *title, const char *desc, bool cur,
                         bool defVal,
                         const std::vector<ConfigConditionV2> &vis,
                         const std::vector<ConfigConditionV2> &en) {
        ConfigNodeV2 n{};
        n.id = key;
        n.key = key;
        n.category = cat;
        n.section = sec;
        n.title = title;
        n.description = desc;
        n.type = ConfigControlTypeV2::Toggle;
        n.defaultValue = defVal ? "true" : "false";
        n.currentValue = cur ? "true" : "false";
        n.hasCurrentValue = true;
        n.visibleWhen = vis;
        n.enabledWhen = en;
        schema.node(std::move(n));
    };

    addSliderFloat(keys::kBlendStrength, "framegen", "Synthesis Tuning",
                   "Temporal Synthesis Strength",
                   "Controls optical-flow temporal synthesis weight.",
                   config.blendStrength, 0.65f, 0.0f, 1.0f, 0.05f, 2, "",
                   fgEnabledCond, fgEnabledCond);
    addSliderFloat(keys::kMotionScale, "framegen", "Synthesis Tuning",
                   "Optical Motion Vector Scale",
                   "Scales camera-to-screen optical flow displacement.",
                   config.motionScale, 1.0f, 0.0f, 2.0f, 0.05f, 2, "x",
                   fgEnabledCond, fgEnabledCond);
    addSliderFloat(keys::kHudProtection, "framegen", "Artifact Protection",
                   "HUD & Crosshair Edge Protection",
                   "Keeps static HUD, crosshair, and hotbar razor-sharp.",
                   config.hudProtection, 0.88f, 0.0f, 1.0f, 0.02f, 2, "",
                   fgEnabledCond, fgEnabledCond);
addSliderFloat(keys::kSceneCutThreshold, "framegen", "Artifact Protection",
                    "Scene-Cut Reset Threshold",
                    "Angular velocity threshold that flushes temporal history.",
                    config.sceneCutThreshold, 42.0f, 10.0f, 120.0f, 2.0f, 1,
                    "deg/f", fgEnabledCond, fgEnabledCond);
    addSliderFloat(keys::kSynthesisScale, "framegen", "Synthesis Tuning",
                    "Synthesis Resolution Scale",
                    "Render synthesis at lower resolution for massive GPU savings. "
                    "0.5 = half resolution (4x fewer pixels), 0.75 = 3/4, 1.0 = native.",
                    config.synthesisScale, 1.0f, 0.25f, 1.0f, 0.05f, 2, "x",
                    fgEnabledCond, fgEnabledCond);
    addSliderFloat(keys::kBlendFalloff, "framegen", "Blending Quality",
                    "Edge Feathering (Blend Falloff)",
                    "Smoothly feathers the transition between interpolated and non-interpolated "
                    "areas (HUD edges, motion boundaries). Higher = smoother transitions, less "
                    "ghosting but slightly softer image. 0.15 recommended.",
                    config.blendFalloff, 0.15f, 0.0f, 0.5f, 0.01f, 2, "",
                    fgEnabledCond, fgEnabledCond);
    addSliderFloat(keys::kHudDetectionSensitivity, "framegen", "Blending Quality",
                    "HUD Detection Sensitivity",
                    "Controls how aggressively HUD elements are detected and protected from "
                    "interpolation. Lower = less ghosting on HUD, higher = more interpolation "
                    "in HUD-adjacent areas. 1.0 = default.",
                    config.hudDetectionSensitivity, 1.0f, 0.5f, 2.0f, 0.05f, 2, "x",
                    fgEnabledCond, fgEnabledCond);
    addSliderFloat(keys::kTemporalSmoothing, "framegen", "Blending Quality",
                    "Temporal Smoothing",
                    "Blends current synthesized frame with previous synthesis to reduce "
                    "flicker and temporal aliasing. 0.0 = off, 1.0 = max smoothing. "
                    "0.3 recommended for natural motion.",
                    config.temporalSmoothing, 0.3f, 0.0f, 1.0f, 0.05f, 2, "",
                    fgEnabledCond, fgEnabledCond);
    addToggle(keys::kMultiSwapStrictMode, "framegen", "MultiSwap Safety",
                    "Strict MultiSwap Mode",
                    "When ON (default), MultiSwap only activates on 90Hz+ displays or with "
                    "Turbo Mode. Prevents massive frame drops on 60Hz devices. Turn OFF at "
                    "your own risk — may cause extreme lag on standard displays.",
                    config.multiSwapStrictMode, true, {}, {});

    // --- Category: camera ---
    {
        ConfigNodeV2 camModeNode{};
        camModeNode.id = keys::kCameraSmoothingMode;
        camModeNode.key = keys::kCameraSmoothingMode;
        camModeNode.category = "camera";
        camModeNode.section = "Filter Algorithm";
        camModeNode.title = "Camera Smoothing Algorithm";
        camModeNode.description =
            "Adaptive Smart suppresses slow-pan micro-jitter while keeping "
            "fast combat flicks zero-latency.";
        camModeNode.type = ConfigControlTypeV2::Choice;
        camModeNode.choiceStyle = ConfigChoiceStyleV2::Radio;
        camModeNode.defaultValue = "3";
        camModeNode.currentValue =
            std::to_string(static_cast<int>(config.cameraSmoothingMode));
        camModeNode.hasCurrentValue = true;
        camModeNode.options = {
            {"0", "Off", "Raw unsmoothed camera input", "", false, "", false},
            {"1", "Exponential (EMA)", "Frame-rate invariant exponential filter", "", false, "", false},
            {"2", "Critically Damped Spring", "Inertial zero-overshoot spring damper", "", false, "", false},
            {"3", "Adaptive Smart (1-Euro)", "Velocity-adaptive cinematic + snappy filter (Recommended)", "", false, "", false},
        };
        schema.node(std::move(camModeNode));
    }

    addSliderFloat(keys::kCameraSmoothness, "camera", "Response Tuning",
                   "Camera Smoothing Strength",
                   "Higher values produce silkier cinematic camera pans.",
                   config.cameraSmoothness, 0.55f, 0.0f, 0.95f, 0.05f, 2, "",
                   camEnabledCond, camEnabledCond);
    addSliderFloat(keys::kCameraResponsiveness, "camera", "Response Tuning",
                   "Fast-Turn Responsiveness Gain",
                   "Controls how fast the filter snaps during rapid flicks.",
                   config.cameraResponsiveness, 1.05f, 0.10f, 2.50f, 0.05f, 2,
                   "x", camEnabledCond, camEnabledCond);
    addSliderFloat(keys::kMicroDeadzone, "camera", "Touch & Gyro Conditioning",
                   "Touch Micro-Jitter Deadzone",
                   "Suppresses direction-reversing touch digitizer noise.",
                   config.microDeadzone, 0.025f, 0.0f, 0.50f, 0.005f, 3, "deg",
                   camEnabledCond, camEnabledCond);
    addSliderFloat(keys::kSpikeFilterDeg, "camera",
                   "Touch & Gyro Conditioning", "Turn-Spike Clamp",
                   "Clamps single-frame multi-touch jump glitches.",
                   config.spikeFilterDeg, 75.0f, 10.0f, 180.0f, 5.0f, 1,
                   "deg/f", camEnabledCond, camEnabledCond);
    addToggle(keys::kFovScaling, "camera", "Touch & Gyro Conditioning",
              "FOV-Aware Zoom Sensitivity Scaling",
              "Automatically steadies camera smoothing when zoomed in.",
              config.fovScaling, true, camEnabledCond, camEnabledCond);

    // --- Category: pacing & HUD Optimizer ---
    {
        ConfigNodeV2 hudOptNode{};
        hudOptNode.id = keys::kHudOptimizerMode;
        hudOptNode.key = keys::kHudOptimizerMode;
        hudOptNode.category = "pacing";
        hudOptNode.section = "HUD Optimizer";
        hudOptNode.title = "HUD Optimizer Mode";
        hudOptNode.description =
            "Eliminates HUD/crosshair input lag, skips redundant UI canvas "
            "redraws, and boosts inventory/chest screen responsiveness.";
        hudOptNode.type = ConfigControlTypeV2::Choice;
        hudOptNode.choiceStyle = ConfigChoiceStyleV2::Segmented;
        hudOptNode.defaultValue = "2";
        hudOptNode.currentValue =
            std::to_string(static_cast<int>(config.hudOptimizerMode));
        hudOptNode.hasCurrentValue = true;
        hudOptNode.options = {
            {"0", "Off", "Standard HUD rendering", "", false, "", false},
            {"1", "Balanced", "Flushes HUD latency during active movement", "", false, "", false},
            {"2", "Ultra Low-Latency", "Maximum HUD & inventory responsiveness (Recommended)", "", false, "", false},
        };
        schema.node(std::move(hudOptNode));
    }

    addToggle(keys::kHudLatencyFlush, "pacing", "HUD Optimizer",
              "HUD Pre-Swap Latency Flush",
              "Flushes GPU command queue before swap so crosshair and hotbar "
              "respond with zero buffered frame delay.",
              config.hudLatencyFlush, true, hudOptEnabledCond,
              hudOptEnabledCond);
    addToggle(keys::kHudStaticScreenBoost, "pacing", "HUD Optimizer",
              "Inventory & GUI Fast-Response Boost",
              "Optimizes frame pacing when inventory, chest, or pause menus "
              "are open.",
              config.hudStaticScreenBoost, true, hudOptEnabledCond,
              hudOptEnabledCond);
    addToggle(keys::kClearScreenOverlays, "pacing", "HUD Optimizer",
              "Remove Center-Screen Overlays & Logos",
              "Keeps the screen 100% clean with zero floating logos or boxes.",
              config.clearScreenOverlays, true, {}, {});

    // Cool toggles - actually do something, VISIBLE in Frame Generation tab (not hidden in pacing)
    addToggle(keys::kTurboMode, "framegen", "Cool Toggles",
              "TURBO 4x Burst",
              "Forces 4x Multi-Swap max FPS burst — actually switches to 4x warp + 0.90 blend, bypasses Auto-Tune. Visible here so you see it.",
              config.turboMode, false, {}, {});
    addToggle(keys::kReflexMode, "framegen", "Cool Toggles",
              "Reflex Low Latency",
              "Bypasses Frame Pacer wait — presents instantly. Actually skips paceForTargetFps, 1ms less latency. Visible here.",
              config.reflexMode, false, {}, {});
    addToggle(keys::kCrystalSharp, "framegen", "Cool Toggles",
              "Crystal Sharp",
              "Extra sharp — forces shader finalWeight ghost protection to 100% sharp. Actually clamps edgeGuard to 1.0. Visible here.",
              config.crystalSharp, true, {}, {});
    addToggle(keys::kKillVignette, "framegen", "Cool Toggles",
              "Vignette Killer",
              "Kills dark vignette overlay + sculk/warden particles for +20% FPS. Actually patches renderVignette to RET. Visible here.",
              config.killVignette, true, {}, {});
    addToggle(keys::kDebugShowGeneratedFrames, "framegen", "Debug",
              "Tint Generated Frames Red",
              "When ON, synthesized frames are tinted red so you can visually confirm frame generation is running. If the screen stays clean, no frames are being generated.",
              config.debugShowGeneratedFrames, false, {}, {});
    addToggle(keys::kEnableMotionBlur, "pacing", "HUD Optimizer",
              "Motion Blur",
              "ON = cinematic blur (may look blurry), OFF = sharp no-blur (recommended for 200 FPS real feel).",
              config.enableMotionBlur, true, {}, {});
    addSliderFloat(keys::kMotionBlurStrength, "pacing", "Motion Blur",
                    "Motion Blur Strength",
                    "Strength of directional motion blur trails. 0.0 = off, 1.0 = max blur. "
                    "Only applies when Motion Blur is ON. Step 0.001 for fine tuning.",
                    config.motionBlurStrength, 0.0f, 0.0f, 1.0f, 0.001f, 3, "",
                    fgEnabledCond, fgEnabledCond);
    addToggle(keys::kPauseWhenStationary, "pacing", "Frame Pacing",
              "Pause When Stationary (AFK)",
              "Automatically pause frame generation after ~0.5s of no camera movement. "
              "Saves GPU power when AFK or standing still. Resumes instantly on movement.",
              config.pauseWhenStationary, true, {}, {});

    addToggle(keys::kAutoTune, "pacing", "Frame Pacing",
              "Smart Auto-Tune Budget Guard",
              "Adapts synthesis workload if GPU frame times spike.",
              config.autoTune, true, {}, {});
    addSliderInt(keys::kTargetFps, "pacing", "Frame Pacing",
                 "Frame Pacer Target FPS (0 = Unlimited)",
                 "Set to 0 for native VSync, or set a custom FPS target.",
                 config.targetFps, 0, 0, 240, 5, "FPS", {}, {});
    addToggle(keys::kShowHud, "pacing", "Optional Telemetry Badge",
              "Show Telemetry Badge (Top-Left)",
              "Off by default so the screen stays completely clean.",
              config.showHud, false, {}, {});

    {
        ConfigNodeV2 styleNode{};
        styleNode.id = keys::kHudStyle;
        styleNode.key = keys::kHudStyle;
        styleNode.category = "pacing";
        styleNode.section = "HUD Overlay";
        styleNode.title = "HUD Layout Style";
        styleNode.description = "Select HUD card information density.";
        styleNode.type = ConfigControlTypeV2::Choice;
        styleNode.choiceStyle = ConfigChoiceStyleV2::Segmented;
        styleNode.defaultValue = "1";
        styleNode.currentValue =
            std::to_string(static_cast<int>(config.hudStyle));
        styleNode.hasCurrentValue = true;
        styleNode.visibleWhen = hudEnabledCond;
        styleNode.enabledWhen = hudEnabledCond;
        styleNode.options = {
            {"0", "Compact", "2-line summary card", "", false, "", false},
            {"1", "Detailed", "4-line full telemetry card", "", false, "", false},
            {"2", "Minimal", "Compact single-line FPS badge", "", false, "", false},
        };
        schema.node(std::move(styleNode));
    }

    addSliderInt(keys::kHudOpacity, "pacing", "HUD Overlay",
                 "HUD Card Opacity", "Background opacity of the HUD card.",
                 config.hudOpacity, 85, 20, 100, 5, "%", hudEnabledCond,
                 hudEnabledCond);

    return schema.toJson();
}

bool loadConfigFromFile(const ConfigFilePath &path,
                        FrameGenConfig &outConfig) {
    if (path.empty()) {
        outConfig.normalize();
        return false;
    }

    std::string fileContents;

#if defined(__ANDROID__)
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        outConfig.normalize();
        return false;
    }

    char buffer[4096];
    bool readOk = true;
    for (;;) {
        ssize_t count = ::read(fd, buffer, sizeof(buffer));
        if (count == 0) break;
        if (count < 0) {
            if (errno == EINTR) continue;
            readOk = false;
            break;
        }
        fileContents.append(buffer, static_cast<std::size_t>(count));
    }
    ::close(fd);

    if (!readOk) {
        outConfig.normalize();
        return false;
    }
#else
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        outConfig.normalize();
        return false;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        outConfig.normalize();
        return false;
    }

    std::ostringstream ss;
    ss << in.rdbuf();
    fileContents = ss.str();
#endif

    std::unordered_map<std::string, std::string> kv;
    if (!parseFlatJsonObject(fileContents, kv)) {
        outConfig.normalize();
        return false;
    }

    auto getInt = [&](const char *k, int &dst) {
        if (auto it = kv.find(k); it != kv.end()) {
            dst = parseIntValue(it->second, dst);
        }
    };
    auto getFloat = [&](const char *k, float &dst) {
        if (auto it = kv.find(k); it != kv.end()) {
            dst = parseFloatValue(it->second, dst);
        }
    };
    auto getBool = [&](const char *k, bool &dst) {
        if (auto it = kv.find(k); it != kv.end()) {
            dst = parseBoolValue(it->second, dst);
        }
    };

    int fileSchemaVersion = 1;
    getInt("schema_version", fileSchemaVersion);

    int presetInt = static_cast<int>(outConfig.preset);
    getInt(keys::kPreset, presetInt);
    outConfig.preset = static_cast<QualityPreset>(presetInt);

    int fgModeInt = static_cast<int>(outConfig.frameGenMode);
    getInt(keys::kFrameGenMode, fgModeInt);
    outConfig.frameGenMode = static_cast<FrameGenMode>(fgModeInt);

    getInt(keys::kFrameGenMultiplier, outConfig.frameGenMultiplier);
    getFloat(keys::kBlendStrength, outConfig.blendStrength);
    getFloat(keys::kMotionScale, outConfig.motionScale);
    getFloat(keys::kHudProtection, outConfig.hudProtection);
    getFloat(keys::kSceneCutThreshold, outConfig.sceneCutThreshold);
    getFloat(keys::kSynthesisScale, outConfig.synthesisScale);

    int camModeInt = static_cast<int>(outConfig.cameraSmoothingMode);
    getInt(keys::kCameraSmoothingMode, camModeInt);
    outConfig.cameraSmoothingMode = static_cast<CameraSmoothingMode>(camModeInt);

    getFloat(keys::kCameraSmoothness, outConfig.cameraSmoothness);
    getFloat(keys::kCameraResponsiveness, outConfig.cameraResponsiveness);
    getFloat(keys::kMicroDeadzone, outConfig.microDeadzone);
    getFloat(keys::kSpikeFilterDeg, outConfig.spikeFilterDeg);
    getBool(keys::kFovScaling, outConfig.fovScaling);

    getBool(keys::kAutoTune, outConfig.autoTune);
    getInt(keys::kTargetFps, outConfig.targetFps);

    int hudOptInt = static_cast<int>(outConfig.hudOptimizerMode);
    getInt(keys::kHudOptimizerMode, hudOptInt);
    outConfig.hudOptimizerMode = static_cast<HudOptimizerMode>(hudOptInt);
    getBool(keys::kHudLatencyFlush, outConfig.hudLatencyFlush);
    getBool(keys::kHudStaticScreenBoost, outConfig.hudStaticScreenBoost);
    getBool(keys::kClearScreenOverlays, outConfig.clearScreenOverlays);
    getBool(keys::kEnableMotionBlur, outConfig.enableMotionBlur);
    getBool(keys::kTurboMode, outConfig.turboMode);
    getBool(keys::kReflexMode, outConfig.reflexMode);
    getBool(keys::kCrystalSharp, outConfig.crystalSharp);
    getBool(keys::kKillVignette, outConfig.killVignette);

    getBool(keys::kShowHud, outConfig.showHud);
    int hudStyleInt = static_cast<int>(outConfig.hudStyle);
    getInt(keys::kHudStyle, hudStyleInt);
    outConfig.hudStyle = static_cast<HudStyle>(hudStyleInt);

    getInt(keys::kHudOpacity, outConfig.hudOpacity);
    getFloat(keys::kHudPosX, outConfig.hudPosX);
    getFloat(keys::kHudPosY, outConfig.hudPosY);

    // Automatically migrate pre-v3 configs to clear any old on-screen HUD/logo
    if (fileSchemaVersion < 3) {
        outConfig.showHud = false;
        outConfig.clearScreenOverlays = true;
        outConfig.enableMotionBlur = false;
        outConfig.hudPosX = 16.0f;
        outConfig.hudPosY = 16.0f;
    }

    outConfig.normalize();
    return true;
}

bool saveConfigToFile(const ConfigFilePath &path,
                      const FrameGenConfig &config) {
    if (path.empty()) return false;

    FrameGenConfig c = config;
    c.normalize();

    std::string payload = "{\n";
    auto field = [&](const char *key, const std::string &value,
                     bool comma = true) {
        payload += "  \"";
        payload += key;
        payload += "\": ";
        payload += value;
        payload += comma ? ",\n" : "\n";
    };
    auto boolean = [](bool value) {
        return std::string(value ? "true" : "false");
    };
    auto integer = [](int value) {
        return std::to_string(value);
    };

    field("schema_version", integer(c.schemaVersion));
    field(keys::kPreset, integer(static_cast<int>(c.preset)));
    field(keys::kFrameGenMode, integer(static_cast<int>(c.frameGenMode)));
    field(keys::kFrameGenMultiplier, integer(c.frameGenMultiplier));
    field(keys::kBlendStrength, formatFloat(c.blendStrength, 4));
    field(keys::kMotionScale, formatFloat(c.motionScale, 4));
    field(keys::kHudProtection, formatFloat(c.hudProtection, 4));
    field(keys::kSceneCutThreshold, formatFloat(c.sceneCutThreshold, 2));
    field(keys::kSynthesisScale, formatFloat(c.synthesisScale, 2));
    field(keys::kCameraSmoothingMode, integer(static_cast<int>(c.cameraSmoothingMode)));
    field(keys::kCameraSmoothness, formatFloat(c.cameraSmoothness, 4));
    field(keys::kCameraResponsiveness, formatFloat(c.cameraResponsiveness, 4));
    field(keys::kMicroDeadzone, formatFloat(c.microDeadzone, 4));
    field(keys::kSpikeFilterDeg, formatFloat(c.spikeFilterDeg, 2));
    field(keys::kFovScaling, boolean(c.fovScaling));
    field(keys::kAutoTune, boolean(c.autoTune));
    field(keys::kTargetFps, integer(c.targetFps));
    field(keys::kHudOptimizerMode, integer(static_cast<int>(c.hudOptimizerMode)));
    field(keys::kHudLatencyFlush, boolean(c.hudLatencyFlush));
    field(keys::kHudStaticScreenBoost, boolean(c.hudStaticScreenBoost));
    field(keys::kClearScreenOverlays, boolean(c.clearScreenOverlays));
    field(keys::kEnableMotionBlur, boolean(c.enableMotionBlur));
    field(keys::kTurboMode, boolean(c.turboMode));
    field(keys::kReflexMode, boolean(c.reflexMode));
    field(keys::kCrystalSharp, boolean(c.crystalSharp));
    field(keys::kKillVignette, boolean(c.killVignette));
    field(keys::kShowHud, boolean(c.showHud));
    field(keys::kHudStyle, integer(static_cast<int>(c.hudStyle)));
    field(keys::kHudOpacity, integer(c.hudOpacity));
    field(keys::kHudPosX, formatFloat(c.hudPosX, 2));
    field(keys::kHudPosY, formatFloat(c.hudPosY, 2), false);
    payload += "}\n";

#if defined(__ANDROID__)
    // Create parent directories without std::filesystem.
    const std::size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && slash > 0) {
        std::string directory = path.substr(0, slash);
        for (std::size_t i = 1; i <= directory.size(); ++i) {
            if (i == directory.size() || directory[i] == '/') {
                std::string part = directory.substr(0, i);
                if (!part.empty() && ::mkdir(part.c_str(), 0700) != 0 &&
                    errno != EEXIST) {
                    return false;
                }
            }
        }
    }

    const std::string tempPath = path + ".tmp";
    int fd = ::open(tempPath.c_str(),
                    O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;

    std::size_t offset = 0;
    bool ok = true;
    while (offset < payload.size()) {
        ssize_t count = ::write(fd, payload.data() + offset,
                                payload.size() - offset);
        if (count < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        if (count == 0) {
            ok = false;
            break;
        }
        offset += static_cast<std::size_t>(count);
    }

    if (::close(fd) != 0) ok = false;
    if (!ok) {
        ::unlink(tempPath.c_str());
        return false;
    }

    if (::rename(tempPath.c_str(), path.c_str()) != 0) {
        ::unlink(path.c_str());
        if (::rename(tempPath.c_str(), path.c_str()) != 0) {
            ::unlink(tempPath.c_str());
            return false;
        }
    }
    return true;
#else
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    return out.good();
#endif
}

// BedrockTools-style ALL toggles JSON - every single toggle under Modules["JNGS Frame Gen"]
nlohmann::json FrameGenConfig::toJson() const {
    FrameGenConfig c = *this;
    c.normalize();
    nlohmann::json j;
    // BedrockTools Module base fields
    j["masterEnabled"] = true;
    j["keybindActive"] = true;
    j["keybind"] = 0;
    j["enabled"] = (c.frameGenMode != FrameGenMode::Off);
    // ALL toggles - every config value
    j["schema_version"] = c.schemaVersion;
    j[keys::kPreset] = static_cast<int>(c.preset);
    j[keys::kFrameGenMode] = static_cast<int>(c.frameGenMode);
    j[keys::kFrameGenMultiplier] = c.frameGenMultiplier;
    j[keys::kBlendStrength] = c.blendStrength;
    j[keys::kMotionScale] = c.motionScale;
    j[keys::kHudProtection] = c.hudProtection;
    j[keys::kSceneCutThreshold] = c.sceneCutThreshold;
    j[keys::kSynthesisScale] = c.synthesisScale;
    j[keys::kCameraSmoothingMode] = static_cast<int>(c.cameraSmoothingMode);
    j[keys::kCameraSmoothness] = c.cameraSmoothness;
    j[keys::kCameraResponsiveness] = c.cameraResponsiveness;
    j[keys::kMicroDeadzone] = c.microDeadzone;
    j[keys::kSpikeFilterDeg] = c.spikeFilterDeg;
    j[keys::kFovScaling] = c.fovScaling;
    j[keys::kAutoTune] = c.autoTune;
    j[keys::kTargetFps] = c.targetFps;
    j[keys::kHudOptimizerMode] = static_cast<int>(c.hudOptimizerMode);
    j[keys::kHudLatencyFlush] = c.hudLatencyFlush;
    j[keys::kHudStaticScreenBoost] = c.hudStaticScreenBoost;
    j[keys::kClearScreenOverlays] = c.clearScreenOverlays;
    j[keys::kEnableMotionBlur] = c.enableMotionBlur;
    j[keys::kMotionBlurStrength] = c.motionBlurStrength;
    j[keys::kPauseWhenStationary] = c.pauseWhenStationary;
    j[keys::kBlendFalloff] = c.blendFalloff;
    j[keys::kHudDetectionSensitivity] = c.hudDetectionSensitivity;
    j[keys::kTemporalSmoothing] = c.temporalSmoothing;
    j[keys::kMultiSwapStrictMode] = c.multiSwapStrictMode;
    j[keys::kShowHud] = c.showHud;
    j[keys::kHudStyle] = static_cast<int>(c.hudStyle);
    j[keys::kHudOpacity] = c.hudOpacity;
    j[keys::kHudPosX] = c.hudPosX;
    j[keys::kHudPosY] = c.hudPosY;
    // Legacy aliases for UI compat
    j["hudPosX"] = c.hudPosX;
    j["hudPosY"] = c.hudPosY;
    j["masterEnabled"] = (c.frameGenMode != FrameGenMode::Off);
    return j;
}

void FrameGenConfig::fromJson(const nlohmann::json& j) {
    auto getInt = [&](const char* k, int& dst) {
        if (j.contains(k) && j[k].is_number_integer()) dst = j[k].get<int>();
        else if (j.contains(k) && j[k].is_number()) dst = static_cast<int>(j[k].get<float>());
    };
    auto getFloat = [&](const char* k, float& dst) {
        if (j.contains(k) && j[k].is_number()) dst = j[k].get<float>();
    };
    auto getBool = [&](const char* k, bool& dst) {
        if (j.contains(k) && j[k].is_boolean()) dst = j[k].get<bool>();
        else if (j.contains(k) && j[k].is_number()) dst = (j[k].get<int>() != 0);
    };
    // Try both direct and string values (BedrockTools saves some as strings)
    auto getIntFlex = [&](const char* k, int& dst) {
        if (j.contains(k)) {
            if (j[k].is_string()) { dst = std::atoi(j[k].get<std::string>().c_str()); }
            else getInt(k, dst);
        }
    };
    auto getFloatFlex = [&](const char* k, float& dst) {
        if (j.contains(k)) {
            if (j[k].is_string()) { dst = std::atof(j[k].get<std::string>().c_str()); }
            else getFloat(k, dst);
        }
    };
    auto getBoolFlex = [&](const char* k, bool& dst) {
        if (j.contains(k)) {
            if (j[k].is_string()) {
                std::string s = j[k].get<std::string>();
                if (s=="true"||s=="1"||s=="on") dst=true;
                else if (s=="false"||s=="0"||s=="off") dst=false;
            } else getBool(k, dst);
        }
    };

    int presetInt = static_cast<int>(preset);
    getIntFlex(keys::kPreset, presetInt);
    preset = static_cast<QualityPreset>(std::clamp(presetInt, 0, 5));

    int modeInt = static_cast<int>(frameGenMode);
    getIntFlex(keys::kFrameGenMode, modeInt);
    frameGenMode = static_cast<FrameGenMode>(std::clamp(modeInt, 0, 2));

    getIntFlex(keys::kFrameGenMultiplier, frameGenMultiplier);
    getFloatFlex(keys::kBlendStrength, blendStrength);
    getFloatFlex(keys::kMotionScale, motionScale);
    getFloatFlex(keys::kHudProtection, hudProtection);
    getFloatFlex(keys::kSceneCutThreshold, sceneCutThreshold);
    getFloatFlex(keys::kSynthesisScale, synthesisScale);

    int camInt = static_cast<int>(cameraSmoothingMode);
    getIntFlex(keys::kCameraSmoothingMode, camInt);
    cameraSmoothingMode = static_cast<CameraSmoothingMode>(std::clamp(camInt, 0, 3));

    getFloatFlex(keys::kCameraSmoothness, cameraSmoothness);
    getFloatFlex(keys::kCameraResponsiveness, cameraResponsiveness);
    getFloatFlex(keys::kMicroDeadzone, microDeadzone);
    getFloatFlex(keys::kSpikeFilterDeg, spikeFilterDeg);
    getBoolFlex(keys::kFovScaling, fovScaling);

    getBoolFlex(keys::kAutoTune, autoTune);
    getIntFlex(keys::kTargetFps, targetFps);

    int hudOptInt = static_cast<int>(hudOptimizerMode);
    getIntFlex(keys::kHudOptimizerMode, hudOptInt);
    hudOptimizerMode = static_cast<HudOptimizerMode>(std::clamp(hudOptInt, 0, 2));
    getBoolFlex(keys::kHudLatencyFlush, hudLatencyFlush);
    getBoolFlex(keys::kHudStaticScreenBoost, hudStaticScreenBoost);
    getBoolFlex(keys::kClearScreenOverlays, clearScreenOverlays);
    getBoolFlex(keys::kEnableMotionBlur, enableMotionBlur);
    getFloatFlex(keys::kMotionBlurStrength, motionBlurStrength);
    getBoolFlex(keys::kPauseWhenStationary, pauseWhenStationary);
    getFloatFlex(keys::kBlendFalloff, blendFalloff);
    getFloatFlex(keys::kHudDetectionSensitivity, hudDetectionSensitivity);
    getFloatFlex(keys::kTemporalSmoothing, temporalSmoothing);
    getBoolFlex(keys::kMultiSwapStrictMode, multiSwapStrictMode);
    getBoolFlex(keys::kTurboMode, turboMode);
    getBoolFlex(keys::kReflexMode, reflexMode);

    getBoolFlex(keys::kShowHud, showHud);
    int hudStyleInt = static_cast<int>(hudStyle);
    getIntFlex(keys::kHudStyle, hudStyleInt);
    hudStyle = static_cast<HudStyle>(std::clamp(hudStyleInt, 0, 2));
    getIntFlex(keys::kHudOpacity, hudOpacity);
    getFloatFlex(keys::kHudPosX, hudPosX);
    getFloatFlex(keys::kHudPosY, hudPosY);
    // legacy aliases
    if (j.contains("hudPosX") && !j.contains(keys::kHudPosX)) getFloatFlex("hudPosX", hudPosX);
    if (j.contains("hudPosY") && !j.contains(keys::kHudPosY)) getFloatFlex("hudPosY", hudPosY);

    // handle masterEnabled from BedrockTools module wrapper
    if (j.contains("masterEnabled") && j["masterEnabled"].is_boolean()) {
        bool me = j["masterEnabled"].get<bool>();
        if (!me) frameGenMode = FrameGenMode::Off;
    }

    normalize();
}

bool saveConfigViaManager(const FrameGenConfig& config) {
    try {
        nlohmann::json j = config.toJson();
        framegen::config::ConfigManager::get().saveConfig(j);
        return true;
    } catch (...) { return false; }
}

bool loadConfigViaManager(FrameGenConfig& outConfig) {
    try {
        nlohmann::json j = framegen::config::ConfigManager::get().loadConfig();
        if (j.is_null() || j.empty()) return false;
        outConfig.fromJson(j);
        return true;
    } catch (...) { return false; }
}

} // namespace framegen
