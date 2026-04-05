#pragma once

#include <chrono>

namespace libera::net {

/**
 * @brief Stores the global timeout configuration for synchronous helpers.
 */
class TimeoutConfig {
public:
    using duration = std::chrono::milliseconds;

    /** Set the process-wide default timeout (clamped to >= 0). */
    static void setDefault(duration timeout) {
        storage() = sanitize(timeout);
    }

    /** Access the current process-wide default timeout. */
    static duration defaultTimeout() {
        return storage();
    }

    /** RAII helper that temporarily overrides the default timeout. */
    class ScopedOverride {
    public:
        explicit ScopedOverride(duration timeout)
        : previous(storage()) {
            storage() = sanitize(timeout);
        }

        ScopedOverride(const ScopedOverride&) = delete;
        ScopedOverride& operator=(const ScopedOverride&) = delete;

        ~ScopedOverride() {
            storage() = previous;
        }

    private:
        duration previous;
    };

private:
    static duration sanitize(duration timeout) {
        return timeout.count() < 0 ? duration::zero() : timeout;
    }

    static duration& storage() {
        static duration timeout{duration{1000}}; // default = 1s
        return timeout;
    }
};

} // namespace libera::net
