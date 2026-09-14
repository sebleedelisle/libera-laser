#pragma once

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

namespace libera::gui::imgui {

struct PluginPanelState {
    std::string lastMessage;
    bool lastMessageIsError = false;
    bool restartHintVisible = false;
    // Settings are edited as drafts so a user can review related changes before
    // applying them to a live plugin or controller.
    std::unordered_map<std::string, std::string> settingEditValues;
    // Remember the value each draft started from. This lets the shared panel
    // detect pending edits and also absorb values changed outside this panel.
    std::unordered_map<std::string, std::string> settingSavedValues;
};

struct PluginPanelCallbacks {
    std::function<std::optional<std::string>()> choosePluginFile;
    std::function<void()> requestRestart;
    std::function<void(const std::string& path)> revealInFileBrowser;
};

struct PluginPanelOptions {
    bool allowInstall = true;
    bool allowRemove = true;
    bool showRestartButton = true;
    bool showRevealButtons = true;
    bool showRuntimeErrors = true;
};

/*
 * Draws reusable plugin-management content into the current ImGui window.
 *
 * The caller owns window placement, native file picking, restart behavior, and
 * any app-specific styling. This panel owns only the shared plugin UI logic.
 */
void DrawPluginManagementPanel(PluginPanelState& state,
                               const PluginPanelCallbacks& callbacks,
                               const PluginPanelOptions& options = {});

/*
 * Draw settings for one stable plugin controller identity. Applications can
 * place this beside their existing controller controls. Edits remain local
 * until the user presses Apply settings; offline values are then saved and
 * applied when the controller next connects.
 */
void DrawPluginControllerSettings(const std::string& pluginType,
                                  const std::string& controllerId,
                                  PluginPanelState& state);

} // namespace libera::gui::imgui
