#pragma once

#include "libera/System.hpp"
#include "libera/core/ControllerCache.hpp"

#include <memory>
#include <string>
#include <utility>

namespace libera::core {

// Helper for the common manager shape where one discovered key maps to one live
// controller instance inside this process.
//
// The derived manager still owns:
// - discovery
// - the manager type name
// - the controller factory
// - transport-specific first-connect behavior
//
// This base owns the repeated lifecycle glue:
// - dynamic_cast from ControllerInfo to the typed info object
// - "reuse existing or create new" cache logic
// - erase-on-failed-first-connect behavior
// - snapshot-and-close shutdown plumbing
template <typename Info, typename Controller, typename Key = std::string>
class ControllerManagerBase : public AbstractControllerManager {
public:
    enum class NewControllerDisposition {
        KeepController,
        DropController
    };

    using ControllerPtr = std::shared_ptr<Controller>;
    using LiveControllerSnapshot = typename ControllerCache<Key, Controller>::Snapshot;

    std::shared_ptr<LaserController> connectController(const ControllerInfo& info) final {
        const auto* typedInfo = dynamic_cast<const Info*>(&info);
        if (!typedInfo) {
            return nullptr;
        }

        const Key key = controllerKey(*typedInfo);
        const auto acquisition = liveControllerCache.getOrCreateIf(
            key,
            [this, typedInfo](const ControllerPtr& controller) {
                return shouldReuseController(*controller, *typedInfo);
            },
            [this, typedInfo] {
                return createController(*typedInfo);
            });

        auto controller = acquisition.controller;
        if (!controller) {
            return nullptr;
        }

        if (acquisition.created) {
            if (prepareNewController(*controller, *typedInfo) ==
                NewControllerDisposition::DropController) {
                // Drop the failed first-acquire instance so a later retry can
                // construct a fresh controller.
                liveControllerCache.erase(key);
                return nullptr;
            }
        } else {
            prepareExistingController(*controller, *typedInfo);
        }

        return controller;
    }

    void closeAll() final {
        beforeCloseControllers();

        {
            auto snapshot = liveControllerCache.snapshotAndClear();
            for (auto& [key, controller] : snapshot) {
                if (!controller) {
                    continue;
                }

                stopController(*controller);
                closeController(key, *controller);
            }
        }

        afterCloseControllers();
    }

protected:
    virtual ~ControllerManagerBase() = default;

    // Most managers key their live-controller cache by ControllerInfo::idValue().
    // Backends with a different reconnect identity such as a USB port path or
    // unit ID can override this.
    virtual Key controllerKey(const Info& info) const {
        return Key(info.idValue());
    }

    virtual ControllerPtr createController(const Info& info) = 0;

    virtual bool shouldReuseController(const Controller& controller,
                                       const Info& info) const {
        (void)controller;
        (void)info;
        return true;
    }

    virtual NewControllerDisposition prepareNewController(Controller& controller,
                                                          const Info& info) = 0;

    virtual void prepareExistingController(Controller& controller,
                                           const Info& info) {
        (void)controller;
        (void)info;
    }

    virtual void beforeCloseControllers() {}
    virtual void afterCloseControllers() {}

    virtual void stopController(Controller& controller) {
        controller.stopThread();
    }

    virtual void closeController(const Key& key, Controller& controller) {
        (void)key;
        (void)controller;
    }

    LiveControllerSnapshot liveControllers() {
        return liveControllerCache.snapshot();
    }

    ControllerPtr findLiveController(const Key& key) {
        return liveControllerCache.findLive(key);
    }

    void dropLiveController(const Key& key) {
        liveControllerCache.erase(key);
    }

private:
    ControllerCache<Key, Controller> liveControllerCache;
};

} // namespace libera::core
