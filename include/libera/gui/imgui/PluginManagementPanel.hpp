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
    // Text inputs need a stable edit buffer across frames. Numeric and choice
    // widgets can render directly from their persisted setting value.
    std::unordered_map<std::string, std::string> settingEditValues;
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
 * place this beside their existing controller controls; offline edits are
 * saved and applied when the controller next connects.
 */
void DrawPluginControllerSettings(const std::string& pluginType,
                                  const std::string& controllerId,
                                  PluginPanelState& state);

} // namespace libera::gui::imgui
