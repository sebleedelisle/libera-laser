#pragma once

#include <filesystem>
#include <string>

namespace libera::plugin {

// Serializes store mutations across Libera applications that share the same
// user plugin directory. The operating system releases the lock on process exit.
class ExclusivePluginFileLock {
public:
    explicit ExclusivePluginFileLock(const std::filesystem::path& path);
    ~ExclusivePluginFileLock();

    ExclusivePluginFileLock(const ExclusivePluginFileLock&) = delete;
    ExclusivePluginFileLock& operator=(const ExclusivePluginFileLock&) = delete;

    bool locked() const { return isLocked; }
    const std::string& error() const { return errorMessage; }

private:
#ifdef _WIN32
    void* handle = nullptr;
#else
    int descriptor = -1;
#endif
    bool isLocked = false;
    std::string errorMessage;
};

} // namespace libera::plugin
