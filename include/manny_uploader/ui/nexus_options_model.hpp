#pragma once

#include "manny_uploader/application/nexus_options_controller.hpp"

#include <optional>
#include <string>

namespace manny_uploader::ui {

struct DonBotOptionsModel {
    std::string status_text;
    std::string diagnostic;
    bool verify_available{};
    bool guild_selection_available{};
    bool enable_toggle_available{};
    bool disconnect_available{};
};

struct TwitchOptionsModel {
    std::string status_text;
    std::string diagnostic;
    std::string test_message_status_text;
    std::string test_message_diagnostic;
    std::optional<std::string> user_code;
    std::optional<std::string> verification_uri;
    bool connect_available{};
    bool enable_toggle_available{};
    bool disconnect_available{};
    bool test_message_available{};
};

struct NexusOptionsModel {
    application::NexusOrdinaryOptions ordinary;
    DonBotOptionsModel donbot;
    TwitchOptionsModel twitch;
    std::string protected_storage_text;
    std::optional<std::string> last_error;
    bool settings_editable{};
    bool save_available{};
    bool command_pending{};
};

[[nodiscard]] NexusOptionsModel
build_nexus_options_model(const application::NexusOptionsSnapshot& snapshot);

} // namespace manny_uploader::ui
