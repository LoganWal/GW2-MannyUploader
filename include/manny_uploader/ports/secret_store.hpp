#pragma once

#include "manny_uploader/support/secret_value.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace manny_uploader::ports {

enum class SecretId : std::uint8_t {
    DpsReportUserToken = 1,
    DonBotGw2ApiKey = 2,
    TwitchOAuthSession = 3,
};

[[nodiscard]] bool is_known_secret_id(SecretId id) noexcept;
[[nodiscard]] std::string_view secret_id_name(SecretId id) noexcept;

enum class SecretStoreErrorCode : std::uint8_t {
    InvalidId,
    EmptySecret,
    SecretTooLarge,
    NotFound,
    UnsupportedEnvironment,
    ProtectionFailed,
    UnprotectionFailed,
    CorruptRecord,
    FileTooLarge,
    ReadFailed,
    DirectoryCreateFailed,
    WriteFailed,
    FlushFailed,
    ReplaceFailed,
    DeleteFailed,
};

struct SecretStoreError {
    SecretStoreErrorCode code;
    std::optional<SecretId> id;
    std::string message;
    std::optional<std::uint32_t> system_error;
};

class ISecretStore {
  public:
    virtual ~ISecretStore() = default;

    [[nodiscard]] virtual std::expected<support::SecretValue, SecretStoreError>
    load(SecretId id) const = 0;
    [[nodiscard]] virtual std::expected<void, SecretStoreError>
    store(SecretId id, const support::SecretValue& value) = 0;
    [[nodiscard]] virtual std::expected<void, SecretStoreError> erase(SecretId id) = 0;
};

} // namespace manny_uploader::ports
