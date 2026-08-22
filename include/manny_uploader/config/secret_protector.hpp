#pragma once

#include "manny_uploader/support/secret_value.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace manny_uploader::config {

inline constexpr std::size_t max_secret_protector_plaintext_bytes = 32U * 1024U;
inline constexpr std::size_t max_secret_protector_ciphertext_bytes = 64U * 1024U;

enum class SecretProtectionErrorCode : std::uint8_t {
    UnsupportedEnvironment,
    InputTooLarge,
    ProtectionFailed,
    UnprotectionFailed,
    OutputTooLarge,
};

struct SecretProtectionError {
    SecretProtectionErrorCode code;
    std::string message;
    std::optional<std::uint32_t> system_error;
};

class ISecretProtector {
  public:
    virtual ~ISecretProtector() = default;

    [[nodiscard]] virtual std::expected<std::vector<std::byte>, SecretProtectionError>
    protect(std::span<const std::byte> plaintext) const = 0;
    [[nodiscard]] virtual std::expected<support::SecretValue, SecretProtectionError>
    unprotect(std::span<const std::byte> ciphertext) const = 0;
};

[[nodiscard]] std::expected<std::unique_ptr<ISecretProtector>, SecretProtectionError>
make_dpapi_secret_protector();

[[nodiscard]] bool is_wine_environment() noexcept;

} // namespace manny_uploader::config
