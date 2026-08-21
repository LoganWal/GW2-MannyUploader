#pragma once

#include "manny_uploader/config/secret_protector.hpp"
#include "manny_uploader/ports/secret_store.hpp"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>

namespace manny_uploader::config {

inline constexpr std::size_t max_secret_value_bytes = 16U * 1024U;
inline constexpr std::size_t max_protected_secret_record_bytes = 64U * 1024U;

class ProtectedFileSecretStore final : public ports::ISecretStore {
  public:
    [[nodiscard]] static std::expected<ProtectedFileSecretStore, ports::SecretStoreError>
    create(std::filesystem::path directory, std::unique_ptr<ISecretProtector> protector);

    ProtectedFileSecretStore(ProtectedFileSecretStore&&) noexcept = default;
    ProtectedFileSecretStore& operator=(ProtectedFileSecretStore&&) noexcept = default;
    ProtectedFileSecretStore(const ProtectedFileSecretStore&) = delete;
    ProtectedFileSecretStore& operator=(const ProtectedFileSecretStore&) = delete;

    [[nodiscard]] std::expected<support::SecretValue, ports::SecretStoreError>
    load(ports::SecretId id) const override;
    [[nodiscard]] std::expected<void, ports::SecretStoreError>
    store(ports::SecretId id, const support::SecretValue& value) override;
    [[nodiscard]] std::expected<void, ports::SecretStoreError> erase(ports::SecretId id) override;

    [[nodiscard]] const std::filesystem::path& directory() const noexcept;
    [[nodiscard]] std::filesystem::path record_path(ports::SecretId id) const;

  private:
    ProtectedFileSecretStore(std::filesystem::path directory,
                             std::unique_ptr<ISecretProtector> protector);

    std::filesystem::path directory_;
    std::unique_ptr<ISecretProtector> protector_;
};

} // namespace manny_uploader::config
