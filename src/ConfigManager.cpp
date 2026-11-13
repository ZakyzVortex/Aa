#include "ConfigManager.hpp"
#include "FrameGenConfig.hpp"

#include <string_view>

#if defined(__ANDROID__)
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <filesystem>
#include <fstream>
#endif

namespace framegen::config {

ConfigManager::~ConfigManager() {
    flush();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping = true;
    }
    m_condition.notify_all();
    if (m_worker.joinable()) m_worker.join();
}

std::string ConfigManager::getConfigPath() const {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_configPath.empty()) return m_configPath;
    }
    // Use JNGS path primary, BedrockTools path as fallback for legacy
    return "/sdcard/games/JNGSFrameGen/config.json";
}

void ConfigManager::setConfigPath(const std::string& path) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_configPath = path;
}

nlohmann::json ConfigManager::loadConfig() {
    const std::string path = getConfigPath();
    const std::string legacyPath = "/sdcard/games/BedrockTools/config.json";
    const std::string flatPath = "/sdcard/games/jngsframegen.json";

    nlohmann::json result;

#if defined(__ANDROID__)
    auto readJson = [](const std::string &filePath, nlohmann::json &out) -> bool {
        int fd = ::open(filePath.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return false;

        std::string contents;
        char buffer[4096];
        bool readOk = true;

        for (;;) {
            const ssize_t count = ::read(fd, buffer, sizeof(buffer));
            if (count == 0) break;
            if (count < 0) {
                if (errno == EINTR) continue;
                readOk = false;
                break;
            }
            contents.append(buffer, static_cast<std::size_t>(count));
        }

        ::close(fd);
        if (!readOk) return false;

        try {
            out = nlohmann::json::parse(contents);
            return true;
        } catch (...) {
            return false;
        }
    };
#else
    auto readJson = [](const std::string &filePath, nlohmann::json &out) -> bool {
        try {
            if (!std::filesystem::exists(filePath)) return false;
            std::ifstream inFile(filePath);
            if (!inFile) return false;
            inFile >> out;
            return true;
        } catch (...) {
            return false;
        }
    };
#endif

    nlohmann::json j;

    if (readJson(path, j)) {
        if (j.contains("Modules") && j["Modules"].contains("JNGS Frame Gen")) {
            return j["Modules"]["JNGS Frame Gen"];
        } else if (j.contains("Modules") && j["Modules"].contains("jngsframegen")) {
            return j["Modules"]["jngsframegen"];
        } else {
            return j;
        }
    }

    if (readJson(legacyPath, j)) {
        if (j.contains("Modules") && j["Modules"].contains("JNGS Frame Gen")) {
            return j["Modules"]["JNGS Frame Gen"];
        }
    }

    if (readJson(flatPath, j)) {
        return j;
    }

    return result;
}

void ConfigManager::saveConfig(const nlohmann::json& frameGenModuleJson) {
    nlohmann::json j;
    // Original LeviFrame Gen toggles ONLY - saved via BedrockTools WAY: Modules["JNGS Frame Gen"] + flat keys stacked for compat
    j["Modules"]["JNGS Frame Gen"] = frameGenModuleJson;
    // Also keep flat keys for backwards compat (original LeviFrame Gen loader)
    for (auto& [k, v] : frameGenModuleJson.items()) {
        j[k] = v;
    }

    std::string path = getConfigPath();
    std::string payload = j.dump(2);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping) return;
        ensureWorkerLocked();
        m_pendingPath = std::move(path);
        m_pendingPayload = std::move(payload);
        m_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
        ++m_queuedGeneration;
        m_hasPending = true;
    }
    m_condition.notify_all();
}

void ConfigManager::load() {
    // This is called on startup - load into global config via FrameGenConfig helpers
    // Actual load is done via loadConfig() -> FrameGenConfig::fromJson
    // Here we just ensure file exists, if not create it
    std::string path = getConfigPath();
#if defined(__ANDROID__)
    const bool configExists = (::access(path.c_str(), F_OK) == 0);
#else
    const bool configExists = std::filesystem::exists(path);
#endif
    if (!configExists) {
        // Trigger a save with defaults to create file
        nlohmann::json defaults;
        // Will be filled by FrameGenMod on first save
        saveConfig(defaults);
        flush();
        return;
    }
}

void ConfigManager::save() {
    // This version is called when ModuleRegistry wants to save all modules
    // For FrameGen we have only one module - the caller should use saveConfig(json)
    // Keep for API compat - if called without args, just flush
    flush();
}

void ConfigManager::flush() {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_worker.joinable() || !m_hasPending) return;
    const std::uint64_t targetGeneration = m_queuedGeneration;
    m_deadline = std::chrono::steady_clock::now();
    m_condition.notify_all();
    m_condition.wait(lock, [&] {
        return m_completedGeneration >= targetGeneration || m_stopping;
    });
}

void ConfigManager::ensureWorkerLocked() {
    if (!m_worker.joinable()) m_worker = std::thread(&ConfigManager::workerLoop, this);
}

void ConfigManager::workerLoop() {
    std::unique_lock<std::mutex> lock(m_mutex);
    while (true) {
        m_condition.wait(lock, [&] { return m_stopping || m_hasPending; });
        if (m_stopping && !m_hasPending) break;

        const std::uint64_t generation = m_queuedGeneration;
        const auto deadline = m_deadline;
        if (!m_stopping && m_condition.wait_until(lock, deadline, [&] {
            return m_stopping || m_queuedGeneration != generation || m_deadline != deadline;
        })) {
            continue;
        }

        std::string path = std::move(m_pendingPath);
        std::string payload = std::move(m_pendingPayload);
        m_hasPending = false;
        lock.unlock();
        writePayload(path, payload);
        lock.lock();
        if (m_completedGeneration < generation) m_completedGeneration = generation;
        m_condition.notify_all();
    }
}

void ConfigManager::writePayload(const std::string& pathValue, const std::string& payload) {
#if defined(__ANDROID__)
    const std::size_t slash = pathValue.find_last_of('/');
    const std::string directory =
        (slash == std::string::npos) ? std::string() : pathValue.substr(0, slash);

    // Create parent directories one level at a time.
    for (std::size_t i = 1; i <= directory.size(); ++i) {
        if (i == directory.size() || directory[i] == '/') {
            const std::string part = directory.substr(0, i);
            if (!part.empty() && ::mkdir(part.c_str(), 0700) != 0 &&
                errno != EEXIST) {
                return;
            }
        }
    }

    const std::string tempPath = pathValue + ".tmp";
    int fd = ::open(tempPath.c_str(),
                    O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return;

    std::size_t offset = 0;
    bool writeOk = true;

    while (offset < payload.size()) {
        const ssize_t count = ::write(
            fd, payload.data() + offset, payload.size() - offset);

        if (count < 0) {
            if (errno == EINTR) continue;
            writeOk = false;
            break;
        }
        if (count == 0) {
            writeOk = false;
            break;
        }
        offset += static_cast<std::size_t>(count);
    }

    if (::close(fd) != 0) writeOk = false;

    if (!writeOk) {
        ::unlink(tempPath.c_str());
        return;
    }

    if (::rename(tempPath.c_str(), pathValue.c_str()) != 0) {
        ::unlink(pathValue.c_str());
        if (::rename(tempPath.c_str(), pathValue.c_str()) != 0) {
            ::unlink(tempPath.c_str());
        }
    }
#else
    std::filesystem::path path(pathValue);
    std::filesystem::path tempPath = path;
    tempPath += ".tmp";

    try {
        if (!path.parent_path().empty()) {
            std::filesystem::create_directories(path.parent_path());
        }
        {
            std::ofstream outFile(tempPath, std::ios::binary | std::ios::trunc);
            outFile.write(payload.data(), static_cast<std::streamsize>(payload.size()));
            outFile.flush();
            if (!outFile) return;
        }

        std::error_code ec;
        std::filesystem::rename(tempPath, path, ec);
        if (ec) {
            std::filesystem::remove(path, ec);
            ec.clear();
            std::filesystem::rename(tempPath, path, ec);
            if (ec) std::filesystem::remove(tempPath, ec);
        }
    } catch (...) {
        std::error_code ec;
        std::filesystem::remove(tempPath, ec);
    }
#endif
}

}
