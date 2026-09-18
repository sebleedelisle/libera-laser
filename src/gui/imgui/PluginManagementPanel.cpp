#include "libera/gui/imgui/PluginManagementPanel.hpp"

#include "imgui.h"
#include "libera/plugin/PluginManagement.hpp"
#include "libera/plugin/PluginSettings.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

namespace libera::gui::imgui {

namespace {

constexpr ImU32 loadedColor = IM_COL32(60, 200, 90, 255);
constexpr ImU32 pendingColor = IM_COL32(230, 180, 70, 255);
constexpr ImU32 failureColor = IM_COL32(230, 110, 110, 255);
constexpr ImU32 neutralColor = IM_COL32(150, 150, 150, 255);

const char* stateLabel(plugin::ManagedPluginState state) {
    using State = plugin::ManagedPluginState;
    switch (state) {
        case State::Loaded:
            return "Loaded";
        case State::NotAPlugin:
            return "Not a Libera plugin";
        case State::FailedLoad:
            return "Failed to load";
        case State::FailedValidation:
            return "Validation failed";
        case State::FailedBackend:
            return "Backend init failed";
        case State::PendingRestart:
            return "Pending restart";
        case State::RemovedPendingRestart:
            return "Removed, restart required";
    }
    return "?";
}

ImU32 stateColor(plugin::ManagedPluginState state) {
    using State = plugin::ManagedPluginState;
    switch (state) {
        case State::Loaded:
            return loadedColor;
        case State::PendingRestart:
        case State::RemovedPendingRestart:
            return pendingColor;
        case State::FailedLoad:
        case State::FailedValidation:
        case State::FailedBackend:
            return failureColor;
        case State::NotAPlugin:
            return neutralColor;
    }
    return neutralColor;
}

std::string formatRuntimeTime(std::chrono::system_clock::time_point timePoint) {
    const auto raw = std::chrono::system_clock::to_time_t(timePoint);
    std::tm localTime{};
#ifdef _WIN32
    localtime_s(&localTime, &raw);
#else
    localtime_r(&raw, &localTime);
#endif

    char buffer[32];
    std::snprintf(buffer,
                  sizeof(buffer),
                  "%02d:%02d:%02d",
                  localTime.tm_hour,
                  localTime.tm_min,
                  localTime.tm_sec);
    return buffer;
}

std::string pluginTitle(const plugin::ManagedPluginInfo& pluginInfo) {
    if (!pluginInfo.displayName.empty()) {
        return pluginInfo.displayName;
    }
    if (!pluginInfo.filename.empty()) {
        return pluginInfo.filename;
    }
    return pluginInfo.path;
}

bool hasRestartRequirement(const std::vector<plugin::ManagedPluginInfo>& plugins) {
    return std::any_of(plugins.begin(),
                       plugins.end(),
                       [](const plugin::ManagedPluginInfo& pluginInfo) {
                           return pluginInfo.restartRequired;
                       });
}

void drawStatusDot(ImU32 color) {
    const float radius = ImGui::GetTextLineHeight() * 0.38f;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    pos.x += radius;
    pos.y += ImGui::GetTextLineHeight() * 0.5f;
    ImGui::GetWindowDrawList()->AddCircleFilled(pos, radius, color, 24);
    ImGui::Dummy(ImVec2(radius * 2.0f + 4.0f, ImGui::GetTextLineHeight()));
}

void drawLabelValue(const char* label, const std::string& value) {
    if (value.empty()) {
        return;
    }
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine();
    ImGui::TextWrapped("%s", value.c_str());
}

void reportSettingResult(const plugin::SettingChangeResult& result,
                         PluginPanelState& state) {
    state.lastMessageIsError = !result.success;
    state.lastMessage = result.message;
}

std::string settingEditKey(const std::string& pluginType,
                           const std::string& controllerId,
                           const std::string& settingKey) {
    return pluginType + "\n" + controllerId + "\n" + settingKey;
}

std::string& prepareSettingDraft(const plugin::Setting& setting,
                                 const std::string& editKey,
                                 PluginPanelState& state) {
    auto [savedIt, savedInserted] = state.settingSavedValues.emplace(
        editKey, setting.value);
    auto [draftIt, draftInserted] = state.settingEditValues.emplace(
        editKey, setting.value);
    (void)draftInserted;

    if (!savedInserted && savedIt->second != setting.value) {
        // Preserve an in-progress edit, but keep untouched controls in sync
        // when another view or a plugin refresh changes the stored value.
        if (draftIt->second == savedIt->second) {
            draftIt->second = setting.value;
        }
        savedIt->second = setting.value;
    }

    return draftIt->second;
}

std::string floatString(double value) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output.precision(std::numeric_limits<double>::max_digits10);
    output << value;
    return output.str();
}

void drawSetting(const plugin::Setting& setting,
                 const std::string& pluginType,
                 const std::string& controllerId,
                 PluginPanelState& state) {
    const auto& definition = setting.definition;
    const auto editKey = settingEditKey(pluginType,
                                        controllerId,
                                        definition.key);
    auto& editValue = prepareSettingDraft(setting, editKey, state);
    ImGui::PushID(definition.key.c_str());

    switch (definition.type) {
        case plugin::SettingType::Bool: {
            bool value = editValue == "true";
            if (ImGui::Checkbox(definition.label.c_str(), &value)) {
                editValue = value ? "true" : "false";
            }
            break;
        }

        case plugin::SettingType::Int: {
            std::int64_t value = 0;
            std::int64_t step = 1;
            try {
                value = std::stoll(editValue);
                if (definition.stepValue) {
                    step = std::stoll(*definition.stepValue);
                }
            } catch (...) {
                value = 0;
                step = 1;
            }
            if (ImGui::InputScalar(definition.label.c_str(),
                                   ImGuiDataType_S64,
                                   &value,
                                   &step)) {
                editValue = std::to_string(value);
            }
            break;
        }

        case plugin::SettingType::Float: {
            double value = 0.0;
            double step = 0.0;
            try {
                value = std::stod(editValue);
                if (definition.stepValue) {
                    step = std::stod(*definition.stepValue);
                }
            } catch (...) {
                value = 0.0;
                step = 0.0;
            }
            const double* stepPointer = step > 0.0 ? &step : nullptr;
            if (ImGui::InputDouble(definition.label.c_str(),
                                   &value,
                                   stepPointer ? *stepPointer : 0.0)) {
                editValue = floatString(value);
            }
            break;
        }

        case plugin::SettingType::String: {
            std::vector<char> buffer(2048, '\0');
            std::snprintf(buffer.data(), buffer.size(), "%s", editValue.c_str());
            if (ImGui::InputText(definition.label.c_str(),
                                 buffer.data(),
                                 buffer.size())) {
                editValue = buffer.data();
            }
            break;
        }

        case plugin::SettingType::Enum: {
            const auto selected = std::find_if(
                definition.choices.begin(),
                definition.choices.end(),
                [&](const plugin::SettingChoice& choice) {
                    return choice.value == editValue;
                });
            const char* preview = selected == definition.choices.end()
                ? editValue.c_str()
                : selected->label.c_str();
            if (ImGui::BeginCombo(definition.label.c_str(), preview)) {
                for (const auto& choice : definition.choices) {
                    const bool isSelected = choice.value == editValue;
                    if (ImGui::Selectable(choice.label.c_str(), isSelected)) {
                        editValue = choice.value;
                    }
                    if (isSelected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            break;
        }
    }

    if (!definition.description.empty()) {
        ImGui::TextDisabled("%s", definition.description.c_str());
    }
    ImGui::PopID();
}

bool hasPendingSettings(const std::vector<plugin::Setting>& settings,
                        const std::string& pluginType,
                        const std::string& controllerId,
                        PluginPanelState& state) {
    return std::any_of(settings.begin(),
                       settings.end(),
                       [&](const plugin::Setting& setting) {
        const auto editKey = settingEditKey(pluginType,
                                            controllerId,
                                            setting.definition.key);
        const auto& draft = prepareSettingDraft(setting, editKey, state);
        return draft != state.settingSavedValues.at(editKey);
    });
}

void applyPendingSettings(const std::vector<plugin::Setting>& settings,
                          const std::string& pluginType,
                          const std::string& controllerId,
                          PluginPanelState& state) {
    std::size_t appliedCount = 0;
    for (const auto& setting : settings) {
        const auto editKey = settingEditKey(pluginType,
                                            controllerId,
                                            setting.definition.key);
        const auto draftIt = state.settingEditValues.find(editKey);
        const auto savedIt = state.settingSavedValues.find(editKey);
        if (draftIt == state.settingEditValues.end() ||
            savedIt == state.settingSavedValues.end() ||
            draftIt->second == savedIt->second) {
            continue;
        }

        const auto result = controllerId.empty()
            ? plugin::setPluginSetting(pluginType,
                                       setting.definition.key,
                                       draftIt->second)
            : plugin::setControllerSetting(pluginType,
                                           controllerId,
                                           setting.definition.key,
                                           draftIt->second);
        if (!result.success) {
            reportSettingResult(
                {false, setting.definition.label + ": " + result.message},
                state);
            return;
        }

        // Mark each successful value clean immediately. If a later setting is
        // rejected, only the unapplied edits remain pending for another try.
        savedIt->second = draftIt->second;
        ++appliedCount;
    }

    if (appliedCount > 0) {
        reportSettingResult(
            {true, appliedCount == 1 ? "Setting applied" : "Settings applied"},
            state);
    }
}

void drawSettings(const std::vector<plugin::Setting>& settings,
                  const std::string& pluginType,
                  const std::string& controllerId,
                  PluginPanelState& state) {
    for (const auto& setting : settings) {
        drawSetting(setting, pluginType, controllerId, state);
    }

    const bool hasPending = hasPendingSettings(settings,
                                               pluginType,
                                               controllerId,
                                               state);
    ImGui::Spacing();
    ImGui::BeginDisabled(!hasPending);
    if (ImGui::Button("Apply settings")) {
        applyPendingSettings(settings, pluginType, controllerId, state);
    }
    ImGui::EndDisabled();
}

void drawPlugin(plugin::ManagedPluginInfo pluginInfo,
                PluginPanelState& state,
                const PluginPanelCallbacks& callbacks,
                const PluginPanelOptions& options) {
    ImGui::PushID(pluginInfo.path.c_str());

    drawStatusDot(stateColor(pluginInfo.state));
    ImGui::SameLine();

    const bool expanded = ImGui::TreeNodeEx(
        "plugin",
        ImGuiTreeNodeFlags_None,
        "%s",
        pluginTitle(pluginInfo).c_str());

    const bool canReveal =
        options.showRevealButtons && callbacks.revealInFileBrowser != nullptr;
    const bool canRemove =
        options.allowRemove && pluginInfo.canRemove && pluginInfo.fileExists;
    if (canReveal || canRemove) {
        ImGui::SameLine();
        const float removeWidth = canRemove ? 86.0f : 0.0f;
        const float revealWidth = canReveal ? 74.0f : 0.0f;
        const float spacing = (canReveal && canRemove)
            ? ImGui::GetStyle().ItemSpacing.x
            : 0.0f;
        const float x = ImGui::GetContentRegionMax().x -
                        removeWidth -
                        revealWidth -
                        spacing;
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), x));

        if (canReveal) {
            if (ImGui::Button("Reveal", ImVec2(revealWidth, 0.0f))) {
                callbacks.revealInFileBrowser(pluginInfo.path);
            }
            if (canRemove) {
                ImGui::SameLine();
            }
        }

        if (canRemove) {
            if (ImGui::Button("Remove", ImVec2(removeWidth, 0.0f))) {
                const auto result = plugin::removePlugin(pluginInfo.pluginId);
                state.lastMessageIsError = !result.success;
                state.lastMessage = result.success
                    ? result.message
                    : "Remove failed: " + result.message;
                if (result.restartRequired) {
                    state.restartHintVisible = true;
                }
            }
        }
    }

    if (expanded) {
        ImGui::Indent();

        ImGui::TextDisabled("State:");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, stateColor(pluginInfo.state));
        ImGui::TextUnformatted(stateLabel(pluginInfo.state));
        ImGui::PopStyleColor();

        drawLabelValue("Plugin ID:", pluginInfo.pluginId);
        drawLabelValue("Version:", pluginInfo.version);
        drawLabelValue("Publisher (unverified):", pluginInfo.vendor);
        drawLabelValue("Controller type:", pluginInfo.typeName);
        drawLabelValue("Package revision:", pluginInfo.packageSha256);
        drawLabelValue("Path:", pluginInfo.path);
        if (!pluginInfo.description.empty()) {
            ImGui::Spacing();
            ImGui::TextWrapped("%s", pluginInfo.description.c_str());
        }

        if (callbacks.revealInFileBrowser &&
            (pluginInfo.readmePath || pluginInfo.licensePath)) {
            ImGui::Spacing();
            if (pluginInfo.readmePath && ImGui::Button("README")) {
                callbacks.revealInFileBrowser(*pluginInfo.readmePath);
            }
            if (pluginInfo.readmePath && pluginInfo.licensePath) ImGui::SameLine();
            if (pluginInfo.licensePath && ImGui::Button("License")) {
                callbacks.revealInFileBrowser(*pluginInfo.licensePath);
            }
        }

        if (pluginInfo.loadError) {
            ImGui::PushStyleColor(ImGuiCol_Text, failureColor);
            ImGui::TextWrapped("Error: %s", pluginInfo.loadError->c_str());
            ImGui::PopStyleColor();
        }

        if (pluginInfo.restartRequired) {
            ImGui::PushStyleColor(ImGuiCol_Text, pendingColor);
            ImGui::TextWrapped("Restart required for this change to take effect.");
            ImGui::PopStyleColor();
        }

        if (pluginInfo.state == plugin::ManagedPluginState::Loaded &&
            !pluginInfo.pluginId.empty()) {
            const auto settings = plugin::pluginSettings(pluginInfo.pluginId);
            if (!settings.empty()) {
                ImGui::Spacing();
                ImGui::TextDisabled("Settings");
                drawSettings(settings, pluginInfo.pluginId, {}, state);
            }
        }

        if (options.showRuntimeErrors && !pluginInfo.runtimeErrors.empty()) {
            ImGui::Spacing();
            ImGui::TextDisabled("Runtime errors");
            for (const auto& runtimeError : pluginInfo.runtimeErrors) {
                const auto timeLabel = formatRuntimeTime(runtimeError.time);
                ImGui::BulletText("%s  %s: %s",
                                  timeLabel.c_str(),
                                  runtimeError.code.c_str(),
                                  runtimeError.message.c_str());
            }
        }

        ImGui::Unindent();
        ImGui::TreePop();
    }

    ImGui::PopID();
}

void drawPluginSection(const char* label,
                       const char* emptyLabel,
                       const std::vector<plugin::ManagedPluginInfo>& plugins,
                       PluginPanelState& state,
                       const PluginPanelCallbacks& callbacks,
                       const PluginPanelOptions& options) {
    ImGui::TextDisabled("%s", label);
    if (plugins.empty()) {
        ImGui::TextDisabled("%s", emptyLabel);
        return;
    }

    for (const auto& pluginInfo : plugins) {
        drawPlugin(pluginInfo, state, callbacks, options);
    }
}

} // namespace

void DrawPluginManagementPanel(PluginPanelState& state,
                               const PluginPanelCallbacks& callbacks,
                               const PluginPanelOptions& options) {
    auto plugins = plugin::listManagedPlugins();
    if (hasRestartRequirement(plugins)) {
        state.restartHintVisible = true;
    }

    ImGui::TextDisabled("User plugin directory:");
    ImGui::SameLine();
    ImGui::TextWrapped("%s", plugin::userPluginDirectory().c_str());
    ImGui::PushStyleColor(ImGuiCol_Text, failureColor);
    ImGui::TextWrapped(
        "Plugins are unsigned native code with the same access as Libera. "
        "Publisher names are unverified; only install packages you trust.");
    ImGui::PopStyleColor();
    ImGui::TextDisabled(
        "Packages are loaded at startup. Install/remove/driver changes may require a restart.");

    if (state.restartHintVisible) {
        if (options.showRestartButton && callbacks.requestRestart) {
            if (ImGui::Button("Restart now", ImVec2(130.0f, 0.0f))) {
                callbacks.requestRestart();
            }
            ImGui::SameLine();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, pendingColor);
        ImGui::TextWrapped("Restart the app for plugin changes to take effect.");
        ImGui::PopStyleColor();
    }

    ImGui::Separator();

    if (options.allowInstall && callbacks.choosePluginFile) {
        if (ImGui::Button("Install new plugin...", ImVec2(190.0f, 0.0f))) {
            const auto picked = callbacks.choosePluginFile();
            if (picked && !picked->empty()) {
                const auto result = plugin::installPlugin(*picked);
                state.lastMessageIsError = !result.success;
                state.lastMessage = result.message;
                if (result.restartRequired) {
                    state.restartHintVisible = true;
                }
            }
        }
    }

    if (!state.lastMessage.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text,
                              state.lastMessageIsError ? failureColor : pendingColor);
        ImGui::TextWrapped("%s", state.lastMessage.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    std::vector<plugin::ManagedPluginInfo> userPlugins;
    std::vector<plugin::ManagedPluginInfo> otherPlugins;
    for (auto& pluginInfo : plugins) {
        if (pluginInfo.source == plugin::ManagedPluginSource::UserPluginDirectory) {
            userPlugins.push_back(std::move(pluginInfo));
        } else {
            otherPlugins.push_back(std::move(pluginInfo));
        }
    }

    drawPluginSection("User plugins",
                      "No user-installed plugins found.",
                      userPlugins,
                      state,
                      callbacks,
                      options);

    if (!otherPlugins.empty()) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        drawPluginSection("Other plugins found at startup",
                          "",
                          otherPlugins,
                          state,
                          callbacks,
                          options);
    }
}

void DrawPluginControllerSettings(const std::string& pluginId,
                                  const std::string& controllerId,
                                  PluginPanelState& state) {
    ImGui::PushID(pluginId.c_str());
    ImGui::PushID(controllerId.c_str());
    drawSettings(plugin::controllerSettings(pluginId, controllerId),
                 pluginId,
                 controllerId,
                 state);
    ImGui::PopID();
    ImGui::PopID();
}

} // namespace libera::gui::imgui
