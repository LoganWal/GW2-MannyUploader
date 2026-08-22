#include "manny_uploader/config/secret_protector.hpp"

#include <array>
#include <limits>
#include <memory>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winapifamily.h>
#include <windows.h>

#include <dpapi.h>
#endif

namespace manny_uploader::config {
namespace {

[[nodiscard]] SecretProtectionError
make_error(SecretProtectionErrorCode code, std::string message,
           std::optional<std::uint32_t> system_error = std::nullopt) {
    return SecretProtectionError{
        .code = code,
        .message = std::move(message),
        .system_error = system_error,
    };
}

#ifdef _WIN32

constexpr std::array entropy{
    std::byte{'G'}, std::byte{'W'}, std::byte{'2'}, std::byte{'-'}, std::byte{'M'}, std::byte{'a'},
    std::byte{'n'}, std::byte{'n'}, std::byte{'y'}, std::byte{'U'}, std::byte{'p'}, std::byte{'l'},
    std::byte{'o'}, std::byte{'a'}, std::byte{'d'}, std::byte{'e'}, std::byte{'r'}, std::byte{'/'},
    std::byte{'s'}, std::byte{'e'}, std::byte{'c'}, std::byte{'r'}, std::byte{'e'}, std::byte{'t'},
    std::byte{'-'}, std::byte{'v'}, std::byte{'1'},
};

[[nodiscard]] DATA_BLOB make_blob(std::span<const std::byte> bytes) noexcept {
    return DATA_BLOB{
        .cbData = static_cast<DWORD>(bytes.size()),
        .pbData = reinterpret_cast<BYTE*>(const_cast<std::byte*>(bytes.data())),
    };
}

class LocalBlob {
  public:
    ~LocalBlob() {
        if (blob_.pbData == nullptr) {
            return;
        }
        SecureZeroMemory(blob_.pbData, blob_.cbData);
        static_cast<void>(LocalFree(blob_.pbData));
    }

    LocalBlob() = default;
    LocalBlob(const LocalBlob&) = delete;
    LocalBlob& operator=(const LocalBlob&) = delete;

    [[nodiscard]] DATA_BLOB* receive() noexcept {
        return &blob_;
    }

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return {reinterpret_cast<const std::byte*>(blob_.pbData), blob_.cbData};
    }

  private:
    DATA_BLOB blob_{};
};

class DpapiSecretProtector final : public ISecretProtector {
  public:
    [[nodiscard]] std::expected<std::vector<std::byte>, SecretProtectionError>
    protect(std::span<const std::byte> plaintext) const override {
        if (plaintext.size() > max_secret_protector_plaintext_bytes ||
            plaintext.size() > std::numeric_limits<DWORD>::max()) {
            return std::unexpected(make_error(SecretProtectionErrorCode::InputTooLarge,
                                              "Credential plaintext exceeds the DPAPI limit"));
        }

        auto input = make_blob(plaintext);
        auto additional_entropy = make_blob(entropy);
        LocalBlob output;
        if (CryptProtectData(&input, L"GW2 Manny Uploader credential", &additional_entropy, nullptr,
                             nullptr, CRYPTPROTECT_UI_FORBIDDEN, output.receive()) == 0) {
            return std::unexpected(make_error(SecretProtectionErrorCode::ProtectionFailed,
                                              "Windows could not protect the credential",
                                              static_cast<std::uint32_t>(GetLastError())));
        }
        if (output.bytes().size() > max_secret_protector_ciphertext_bytes) {
            return std::unexpected(make_error(SecretProtectionErrorCode::OutputTooLarge,
                                              "Protected credential exceeds the storage limit"));
        }
        return std::vector<std::byte>{output.bytes().begin(), output.bytes().end()};
    }

    [[nodiscard]] std::expected<support::SecretValue, SecretProtectionError>
    unprotect(std::span<const std::byte> ciphertext) const override {
        if (ciphertext.size() > max_secret_protector_ciphertext_bytes ||
            ciphertext.size() > std::numeric_limits<DWORD>::max()) {
            return std::unexpected(make_error(SecretProtectionErrorCode::InputTooLarge,
                                              "Protected credential exceeds the DPAPI limit"));
        }

        auto input = make_blob(ciphertext);
        auto additional_entropy = make_blob(entropy);
        LocalBlob output;
        if (CryptUnprotectData(&input, nullptr, &additional_entropy, nullptr, nullptr,
                               CRYPTPROTECT_UI_FORBIDDEN, output.receive()) == 0) {
            return std::unexpected(make_error(SecretProtectionErrorCode::UnprotectionFailed,
                                              "Windows could not unprotect the credential",
                                              static_cast<std::uint32_t>(GetLastError())));
        }
        if (output.bytes().size() > max_secret_protector_plaintext_bytes) {
            return std::unexpected(make_error(SecretProtectionErrorCode::OutputTooLarge,
                                              "Unprotected credential exceeds the storage limit"));
        }
        return support::SecretValue{
            std::vector<std::byte>{output.bytes().begin(), output.bytes().end()}};
    }
};

[[nodiscard]] bool is_running_under_wine() noexcept {
    const auto module = GetModuleHandleW(L"ntdll.dll");
    return module != nullptr && GetProcAddress(module, "wine_get_version") != nullptr;
}

#endif

} // namespace

std::expected<std::unique_ptr<ISecretProtector>, SecretProtectionError>
make_dpapi_secret_protector() {
#ifdef _WIN32
    return std::make_unique<DpapiSecretProtector>();
#else
    return std::unexpected(make_error(SecretProtectionErrorCode::UnsupportedEnvironment,
                                      "DPAPI credential storage requires native Windows"));
#endif
}

bool is_wine_environment() noexcept {
#ifdef _WIN32
    return is_running_under_wine();
#else
    return false;
#endif
}

} // namespace manny_uploader::config
