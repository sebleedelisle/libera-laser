#pragma once

#include <memory>
#include <unordered_map>
#include <utility>

namespace libera::core {

// Remove expired weak pointers and report whether any live controllers remain.
template <typename Map>
bool pruneExpiredActiveControllers(Map& activeControllers) {
    bool hasActive = false;
    for (auto it = activeControllers.begin(); it != activeControllers.end();) {
        if (it->second.expired()) {
            it = activeControllers.erase(it);
        } else {
            hasActive = true;
            ++it;
        }
    }
    return hasActive;
}

// Fetch an existing controller from a weak-pointer map, or create and store one.
template <typename Map, typename Key, typename Factory>
auto getOrCreateActiveController(
    Map& activeControllers,
    const Key& key,
    Factory&& createController,
    bool* created = nullptr)
    -> std::shared_ptr<typename Map::mapped_type::element_type> {
    if (created) {
        *created = false;
    }

    auto it = activeControllers.find(key);
    if (it != activeControllers.end()) {
        if (auto existing = it->second.lock()) {
            return existing;
        }
        activeControllers.erase(it);
    }

    auto controller = createController();
    if (!controller) {
        return controller;
    }

    activeControllers.insert_or_assign(key, controller);
    if (created) {
        *created = true;
    }
    return controller;
}

// Turn a weak-pointer map into a strong-pointer snapshot, then clear the map.
// Managers use this during shutdown so they can stop/close controllers without
// holding a lock for the whole shutdown loop.
template <typename Map>
auto snapshotActiveControllersAndClear(Map& activeControllers)
    -> std::unordered_map<typename Map::key_type, std::shared_ptr<typename Map::mapped_type::element_type>> {
    std::unordered_map<typename Map::key_type, std::shared_ptr<typename Map::mapped_type::element_type>> snapshot;
    snapshot.reserve(activeControllers.size());
    for (auto& [id, weakController] : activeControllers) {
        if (auto controller = weakController.lock()) {
            snapshot.emplace(id, std::move(controller));
        }
    }
    activeControllers.clear();
    return snapshot;
}

} // namespace libera::core
