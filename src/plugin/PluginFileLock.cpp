#include "PluginFileLock.hpp"

#include <cerrno>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace libera::plugin {

ExclusivePluginFileLock::ExclusivePluginFileLock(
    const std::filesystem::path& path) {
#ifdef _WIN32
    HANDLE file = CreateFileW(path.c_str(),
                              GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr,
                              OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        errorMessage = "Failed to open the plugin store lock (Windows error " +
                       std::to_string(GetLastError()) + ")";
        return;
    }
    handle = file;
    OVERLAPPED range{};
    if (!LockFileEx(file,
                    LOCKFILE_EXCLUSIVE_LOCK,
                    0,
                    MAXDWORD,
                    MAXDWORD,
                    &range)) {
        errorMessage = "Failed to lock the plugin store (Windows error " +
                       std::to_string(GetLastError()) + ")";
        CloseHandle(file);
        handle = nullptr;
        return;
    }
    isLocked = true;
#else
    descriptor = open(path.c_str(), O_CREAT | O_RDWR, 0600);
    if (descriptor < 0) {
        errorMessage = "Failed to open the plugin store lock: " +
                       std::string(std::strerror(errno));
        return;
    }
    if (flock(descriptor, LOCK_EX) != 0) {
        errorMessage = "Failed to lock the plugin store: " +
                       std::string(std::strerror(errno));
        close(descriptor);
        descriptor = -1;
        return;
    }
    isLocked = true;
#endif
}

ExclusivePluginFileLock::~ExclusivePluginFileLock() {
#ifdef _WIN32
    if (!handle) return;
    OVERLAPPED range{};
    if (isLocked) {
        UnlockFileEx(static_cast<HANDLE>(handle),
                     0,
                     MAXDWORD,
                     MAXDWORD,
                     &range);
    }
    CloseHandle(static_cast<HANDLE>(handle));
#else
    if (descriptor < 0) return;
    if (isLocked) flock(descriptor, LOCK_UN);
    close(descriptor);
#endif
}

} // namespace libera::plugin
