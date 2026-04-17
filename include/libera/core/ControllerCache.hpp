#pragma once

#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace libera::core {

// Small helper that keeps one weakly-owned controller cache per manager.
//
// Managers use this to:
// - reuse an existing live controller for one stable discovery key
// - take a strong snapshot for discovery or shutdown work
// - drop stale entries automatically when a weak pointer has expired
template <typename Key, typename Controller>
class ControllerCache {
public:
    struct GetOrCreateResult {
        std::shared_ptr<Controller> controller;
        bool created = false;
    };

    using Snapshot = std::unordered_map<Key, std::shared_ptr<Controller>>;

    ControllerCache() = default;
    ControllerCache(const ControllerCache&) = delete;
    ControllerCache& operator=(const ControllerCache&) = delete;

    std::shared_ptr<Controller> findLive(const Key& key) {
        std::lock_guard lock(mutex);
        return findLiveLocked(key);
    }

    template <typename Factory>
    GetOrCreateResult getOrCreate(const Key& key, Factory&& createController) {
        return getOrCreateIf(
            key,
            [](const std::shared_ptr<Controller>&) {
                return true;
            },
            std::forward<Factory>(createController));
    }

    template <typename ReusePredicate, typename Factory>
    GetOrCreateResult getOrCreateIf(const Key& key,
                                    ReusePredicate&& shouldReuseExisting,
                                    Factory&& createController) {
        std::lock_guard lock(mutex);

        auto it = controllers.find(key);
        if (it != controllers.end()) {
            if (auto existing = it->second.lock()) {
                if (shouldReuseExisting(existing)) {
                    return {std::move(existing), false};
                }

                controllers.erase(it);
            } else {
                controllers.erase(it);
            }
        }

        auto controller = createController();
        if (!controller) {
            return {};
        }

        controllers.insert_or_assign(key, controller);
        return {std::move(controller), true};
    }

    void erase(const Key& key) {
        std::lock_guard lock(mutex);
        controllers.erase(key);
    }

    Snapshot snapshot() {
        std::lock_guard lock(mutex);
        return snapshotLocked(false);
    }

    Snapshot snapshotAndClear() {
        std::lock_guard lock(mutex);
        return snapshotLocked(true);
    }

private:
    std::shared_ptr<Controller> findLiveLocked(const Key& key) {
        auto it = controllers.find(key);
        if (it == controllers.end()) {
            return nullptr;
        }

        if (auto controller = it->second.lock()) {
            return controller;
        }

        controllers.erase(it);
        return nullptr;
    }

    Snapshot snapshotLocked(bool clearAfterSnapshot) {
        Snapshot snapshot;
        snapshot.reserve(controllers.size());

        for (auto it = controllers.begin(); it != controllers.end();) {
            if (auto controller = it->second.lock()) {
                snapshot.emplace(it->first, std::move(controller));
                ++it;
                continue;
            }

            it = controllers.erase(it);
        }

        if (clearAfterSnapshot) {
            controllers.clear();
        }

        return snapshot;
    }

    std::mutex mutex;
    std::unordered_map<Key, std::weak_ptr<Controller>> controllers;
};

} // namespace libera::core
