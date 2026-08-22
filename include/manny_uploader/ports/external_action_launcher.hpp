#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace manny_uploader::ports {

enum class ExternalActionErrorCode : std::uint8_t {
    InvalidTarget,
    LaunchFailed,
};

struct ExternalActionError {
    ExternalActionErrorCode code;
    std::string message;
};

class IExternalActionLauncher {
  public:
    virtual ~IExternalActionLauncher() = default;

    [[nodiscard]] virtual std::expected<void, ExternalActionError>
    open_url(std::string_view url) = 0;
    [[nodiscard]] virtual std::expected<void, ExternalActionError>
    open_directory(const std::filesystem::path& directory) = 0;
};

} // namespace manny_uploader::ports
