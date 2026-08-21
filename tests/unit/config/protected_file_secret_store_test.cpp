#include "manny_uploader/config/protected_file_secret_store.hpp"
#include "manny_uploader/support/atomic_file.hpp"
#include "support/test_suite.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace manny_uploader::test {
namespace {

using config::ProtectedFileSecretStore;
using ports::SecretId;
using ports::SecretStoreErrorCode;
using support::SecretValue;

struct ProtectorState {
    bool fail_protect{};
    bool fail_unprotect{};
    bool oversized_output{};
    std::size_t protect_calls{};
    std::size_t unprotect_calls{};
};

class XorSecretProtector final : public config::ISecretProtector {
  public:
    explicit XorSecretProtector(std::shared_ptr<ProtectorState> state) : state_{std::move(state)} {}

    [[nodiscard]] std::expected<std::vector<std::byte>, config::SecretProtectionError>
    protect(std::span<const std::byte> plaintext) const override {
        ++state_->protect_calls;
        if (state_->fail_protect) {
            return std::unexpected(config::SecretProtectionError{
                .code = config::SecretProtectionErrorCode::ProtectionFailed,
                .message = "Test protector rejected the credential",
                .system_error = 1234,
            });
        }
        if (state_->oversized_output) {
            return std::vector<std::byte>(config::max_protected_secret_record_bytes + 1,
                                          std::byte{0xa5});
        }

        std::vector<std::byte> ciphertext;
        ciphertext.reserve(plaintext.size() + 1);
        ciphertext.push_back(std::byte{0x5a});
        for (const auto value : plaintext) {
            ciphertext.push_back(value ^ std::byte{0xa5});
        }
        return ciphertext;
    }

    [[nodiscard]] std::expected<SecretValue, config::SecretProtectionError>
    unprotect(std::span<const std::byte> ciphertext) const override {
        ++state_->unprotect_calls;
        if (state_->fail_unprotect) {
            return std::unexpected(config::SecretProtectionError{
                .code = config::SecretProtectionErrorCode::UnprotectionFailed,
                .message = "Test protector could not decrypt the credential",
                .system_error = 5678,
            });
        }
        if (ciphertext.empty() || ciphertext.front() != std::byte{0x5a}) {
            return std::unexpected(config::SecretProtectionError{
                .code = config::SecretProtectionErrorCode::UnprotectionFailed,
                .message = "Test protected record is malformed",
                .system_error = std::nullopt,
            });
        }

        std::vector<std::byte> plaintext;
        plaintext.reserve(ciphertext.size() - 1);
        for (const auto value : ciphertext.subspan(1)) {
            plaintext.push_back(value ^ std::byte{0xa5});
        }
        return SecretValue{std::move(plaintext)};
    }

  private:
    std::shared_ptr<ProtectorState> state_;
};

class TempSecretTree {
  public:
    TempSecretTree() {
        static std::uint64_t sequence{};
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = std::filesystem::temp_directory_path() /
                ("manny-secrets-" + std::to_string(timestamp) + "-" + std::to_string(++sequence));
        std::error_code error;
        if (!std::filesystem::create_directories(root_, error) || error) {
            throw std::runtime_error{"Could not create protected-secret test directory"};
        }
    }

    ~TempSecretTree() {
        std::error_code error;
        static_cast<void>(std::filesystem::remove_all(root_, error));
    }

    TempSecretTree(const TempSecretTree&) = delete;
    TempSecretTree& operator=(const TempSecretTree&) = delete;

    [[nodiscard]] std::filesystem::path secret_directory() const {
        return root_ / "nested" / "credentials";
    }

    [[nodiscard]] std::filesystem::path path(std::string_view filename) const {
        return root_ / filename;
    }

    void write(const std::filesystem::path& path, std::span<const std::byte> contents) const {
        std::error_code error;
        static_cast<void>(std::filesystem::create_directories(path.parent_path(), error));
        if (error) {
            throw std::runtime_error{"Could not create protected-secret fixture directory"};
        }
        std::ofstream stream{path, std::ios::binary | std::ios::trunc};
        stream.write(reinterpret_cast<const char*>(contents.data()),
                     static_cast<std::streamsize>(contents.size()));
        if (!stream) {
            throw std::runtime_error{"Could not write protected-secret fixture"};
        }
    }

    [[nodiscard]] std::vector<std::byte> read(const std::filesystem::path& path) const {
        std::ifstream stream{path, std::ios::binary};
        std::vector<char> characters{std::istreambuf_iterator<char>{stream},
                                     std::istreambuf_iterator<char>{}};
        const auto bytes = std::as_bytes(std::span{characters});
        return {bytes.begin(), bytes.end()};
    }

  private:
    std::filesystem::path root_;
};

struct StoreFixture {
    TempSecretTree tree;
    std::shared_ptr<ProtectorState> protector_state{std::make_shared<ProtectorState>()};
    ProtectedFileSecretStore store;

    StoreFixture() : store{make_store(tree.secret_directory(), protector_state)} {}

    [[nodiscard]] static ProtectedFileSecretStore
    make_store(const std::filesystem::path& directory,
               const std::shared_ptr<ProtectorState>& state) {
        auto result = ProtectedFileSecretStore::create(directory,
                                                       std::make_unique<XorSecretProtector>(state));
        if (!result) {
            throw std::runtime_error{"Could not construct protected-secret store fixture"};
        }
        return std::move(*result);
    }
};

[[nodiscard]] bool contains_bytes(std::span<const std::byte> haystack,
                                  std::span<const std::byte> needle) {
    return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end()) !=
           haystack.end();
}

[[nodiscard]] bool message_contains(const ports::SecretStoreError& error, std::string_view marker) {
    return error.message.find(marker) != std::string::npos;
}

void secret_value_and_identifier_tests(TestSuite& suite) {
    static_assert(!std::is_copy_constructible_v<SecretValue>);
    static_assert(!std::is_copy_assignable_v<SecretValue>);
    static_assert(std::is_nothrow_move_constructible_v<SecretValue>);
    static_assert(std::is_nothrow_move_assignable_v<SecretValue>);

    auto first = SecretValue::from_text("alpha-secret");
    auto equal = SecretValue::from_text("alpha-secret");
    auto different = SecretValue::from_text("beta-secret");
    MANNY_CHECK(suite, first == equal);
    MANNY_CHECK(suite, !(first == different));
    MANNY_CHECK(suite, first.size() == 12);
    MANNY_CHECK(suite, !first.empty());
    first.clear();
    MANNY_CHECK(suite, first.empty());

    MANNY_CHECK(suite, ports::is_known_secret_id(SecretId::DpsReportUserToken));
    MANNY_CHECK(suite, ports::is_known_secret_id(SecretId::DonBotGw2ApiKey));
    MANNY_CHECK(suite, ports::is_known_secret_id(SecretId::TwitchOAuthSession));
    MANNY_CHECK(suite, !ports::is_known_secret_id(static_cast<SecretId>(255)));
    MANNY_CHECK(suite,
                ports::secret_id_name(SecretId::DpsReportUserToken) == "dps.report user token");
    MANNY_CHECK(suite, ports::secret_id_name(static_cast<SecretId>(255)) == "unknown credential");
}

void creation_and_missing_tests(TestSuite& suite) {
    auto empty_directory = ProtectedFileSecretStore::create(
        {}, std::make_unique<XorSecretProtector>(std::make_shared<ProtectorState>()));
    MANNY_CHECK(suite, !empty_directory.has_value());
    MANNY_CHECK(suite, empty_directory.error().code == SecretStoreErrorCode::DirectoryCreateFailed);
    MANNY_CHECK(suite, !empty_directory.error().id.has_value());

    auto null_protector = ProtectedFileSecretStore::create("credentials", nullptr);
    MANNY_CHECK(suite, !null_protector.has_value());
    MANNY_CHECK(suite, null_protector.error().code == SecretStoreErrorCode::UnsupportedEnvironment);

    StoreFixture fixture;
    MANNY_CHECK(suite, fixture.store.directory() == fixture.tree.secret_directory());
    const auto missing = fixture.store.load(SecretId::DpsReportUserToken);
    MANNY_CHECK(suite, !missing.has_value());
    MANNY_CHECK(suite, missing.error().code == SecretStoreErrorCode::NotFound);
    MANNY_CHECK(suite, missing.error().id == SecretId::DpsReportUserToken);
}

void round_trip_and_replacement_tests(TestSuite& suite) {
    StoreFixture fixture;
    constexpr std::array ids{
        SecretId::DpsReportUserToken,
        SecretId::DonBotGw2ApiKey,
        SecretId::TwitchOAuthSession,
    };

    for (const auto id : ids) {
        const auto marker = std::string{"unique-credential-marker-"} +
                            std::to_string(static_cast<std::uint32_t>(id));
        const auto secret = SecretValue::from_text(marker);
        MANNY_CHECK(suite, fixture.store.store(id, secret).has_value());
        MANNY_CHECK(suite, std::filesystem::exists(fixture.store.record_path(id)));
        MANNY_CHECK(suite, !std::filesystem::exists(
                               support::atomic_temporary_path(fixture.store.record_path(id))));
        MANNY_CHECK(suite,
                    !std::filesystem::exists(fixture.store.record_path(id).string() + ".bak"));

        const auto protected_bytes = fixture.tree.read(fixture.store.record_path(id));
        MANNY_CHECK(suite, !contains_bytes(protected_bytes, secret.bytes()));
        const auto loaded = fixture.store.load(id);
        MANNY_CHECK(suite, loaded.has_value());
        MANNY_CHECK(suite, *loaded == secret);
    }

    const auto old_secret = SecretValue::from_text("old-refresh-token-marker");
    const auto new_secret = SecretValue::from_text("new-refresh-token-marker");
    MANNY_CHECK(suite, fixture.store.store(SecretId::TwitchOAuthSession, old_secret).has_value());
    MANNY_CHECK(suite, fixture.store.store(SecretId::TwitchOAuthSession, new_secret).has_value());
    const auto loaded = fixture.store.load(SecretId::TwitchOAuthSession);
    MANNY_CHECK(suite, loaded.has_value());
    MANNY_CHECK(suite, *loaded == new_secret);
    const auto protected_bytes =
        fixture.tree.read(fixture.store.record_path(SecretId::TwitchOAuthSession));
    MANNY_CHECK(suite, !contains_bytes(protected_bytes, old_secret.bytes()));
    MANNY_CHECK(suite, !contains_bytes(protected_bytes, new_secret.bytes()));
}

void validation_and_failure_tests(TestSuite& suite) {
    StoreFixture fixture;
    const auto invalid_id = static_cast<SecretId>(255);
    const auto valid = SecretValue::from_text("do-not-leak-this-marker");
    const SecretValue empty;
    MANNY_CHECK(suite, fixture.store.store(SecretId::DpsReportUserToken, empty).error().code ==
                           SecretStoreErrorCode::EmptySecret);

    const SecretValue oversized{
        std::vector<std::byte>(config::max_secret_value_bytes + 1, std::byte{0x42})};
    MANNY_CHECK(suite, fixture.store.store(SecretId::DpsReportUserToken, oversized).error().code ==
                           SecretStoreErrorCode::SecretTooLarge);
    MANNY_CHECK(suite, fixture.store.store(invalid_id, valid).error().code ==
                           SecretStoreErrorCode::InvalidId);
    MANNY_CHECK(suite,
                fixture.store.load(invalid_id).error().code == SecretStoreErrorCode::InvalidId);
    MANNY_CHECK(suite,
                fixture.store.erase(invalid_id).error().code == SecretStoreErrorCode::InvalidId);

    fixture.protector_state->fail_protect = true;
    const auto protect_failure = fixture.store.store(SecretId::DpsReportUserToken, valid);
    MANNY_CHECK(suite, !protect_failure.has_value());
    MANNY_CHECK(suite, protect_failure.error().code == SecretStoreErrorCode::ProtectionFailed);
    MANNY_CHECK(suite, protect_failure.error().system_error == 1234);
    MANNY_CHECK(suite, !message_contains(protect_failure.error(), "do-not-leak-this-marker"));

    fixture.protector_state->fail_protect = false;
    MANNY_CHECK(suite, fixture.store.store(SecretId::DpsReportUserToken, valid).has_value());
    fixture.protector_state->fail_unprotect = true;
    const auto unprotect_failure = fixture.store.load(SecretId::DpsReportUserToken);
    MANNY_CHECK(suite, !unprotect_failure.has_value());
    MANNY_CHECK(suite, unprotect_failure.error().code == SecretStoreErrorCode::UnprotectionFailed);
    MANNY_CHECK(suite, unprotect_failure.error().system_error == 5678);
    MANNY_CHECK(suite, !message_contains(unprotect_failure.error(), "do-not-leak-this-marker"));

    fixture.protector_state->fail_unprotect = false;
    fixture.protector_state->oversized_output = true;
    const auto oversized_record = fixture.store.store(SecretId::DonBotGw2ApiKey, valid);
    MANNY_CHECK(suite, !oversized_record.has_value());
    MANNY_CHECK(suite, oversized_record.error().code == SecretStoreErrorCode::FileTooLarge);
}

void corruption_and_substitution_tests(TestSuite& suite) {
    StoreFixture fixture;
    const auto secret = SecretValue::from_text("corruption-check-marker");
    const auto dps_path = fixture.store.record_path(SecretId::DpsReportUserToken);
    MANNY_CHECK(suite, fixture.store.store(SecretId::DpsReportUserToken, secret).has_value());

    auto corrupted = fixture.tree.read(dps_path);
    corrupted.back() ^= std::byte{0x01};
    fixture.tree.write(dps_path, corrupted);
    auto loaded = fixture.store.load(SecretId::DpsReportUserToken);
    MANNY_CHECK(suite, !loaded.has_value());
    MANNY_CHECK(suite, loaded.error().code == SecretStoreErrorCode::CorruptRecord);
    MANNY_CHECK(suite, !message_contains(loaded.error(), "corruption-check-marker"));

    MANNY_CHECK(suite, fixture.store.store(SecretId::DpsReportUserToken, secret).has_value());
    const auto valid_dps_record = fixture.tree.read(dps_path);
    fixture.tree.write(fixture.store.record_path(SecretId::DonBotGw2ApiKey), valid_dps_record);
    loaded = fixture.store.load(SecretId::DonBotGw2ApiKey);
    MANNY_CHECK(suite, !loaded.has_value());
    MANNY_CHECK(suite, loaded.error().code == SecretStoreErrorCode::CorruptRecord);

    fixture.tree.write(
        fixture.store.record_path(SecretId::TwitchOAuthSession),
        std::vector<std::byte>(config::max_protected_secret_record_bytes + 1, std::byte{0x42}));
    loaded = fixture.store.load(SecretId::TwitchOAuthSession);
    MANNY_CHECK(suite, !loaded.has_value());
    MANNY_CHECK(suite, loaded.error().code == SecretStoreErrorCode::FileTooLarge);
}

void atomic_failure_and_erase_tests(TestSuite& suite) {
    StoreFixture fixture;
    const auto old_secret = SecretValue::from_text("preserved-old-secret");
    const auto new_secret = SecretValue::from_text("rejected-new-secret");
    const auto id = SecretId::DpsReportUserToken;
    const auto path = fixture.store.record_path(id);
    const auto temporary = support::atomic_temporary_path(path);
    MANNY_CHECK(suite, fixture.store.store(id, old_secret).has_value());

    std::error_code error;
    MANNY_CHECK(suite, std::filesystem::create_directory(temporary, error));
    MANNY_CHECK(suite, !error);
    const auto failed = fixture.store.store(id, new_secret);
    MANNY_CHECK(suite, !failed.has_value());
    MANNY_CHECK(suite, failed.error().code == SecretStoreErrorCode::WriteFailed);
    static_cast<void>(std::filesystem::remove_all(temporary, error));
    MANNY_CHECK(suite, !error);
    const auto loaded = fixture.store.load(id);
    MANNY_CHECK(suite, loaded.has_value());
    MANNY_CHECK(suite, *loaded == old_secret);

    const std::array stale_bytes{std::byte{0x01}, std::byte{0x02}};
    fixture.tree.write(temporary, stale_bytes);
    MANNY_CHECK(suite, fixture.store.erase(id).has_value());
    MANNY_CHECK(suite, !std::filesystem::exists(path));
    MANNY_CHECK(suite, !std::filesystem::exists(temporary));
    MANNY_CHECK(suite, fixture.store.erase(id).has_value());
    MANNY_CHECK(suite, fixture.store.load(id).error().code == SecretStoreErrorCode::NotFound);
}

void invalid_directory_test(TestSuite& suite) {
    TempSecretTree tree;
    const auto directory_path = tree.path("not-a-directory");
    const std::array file_contents{std::byte{0x01}};
    tree.write(directory_path, file_contents);
    auto store = StoreFixture::make_store(directory_path, std::make_shared<ProtectorState>());
    const auto secret = SecretValue::from_text("directory-failure-marker");
    const auto result = store.store(SecretId::DpsReportUserToken, secret);
    MANNY_CHECK(suite, !result.has_value());
    MANNY_CHECK(suite, result.error().code == SecretStoreErrorCode::DirectoryCreateFailed);
    MANNY_CHECK(suite, !message_contains(result.error(), "directory-failure-marker"));
}

void dpapi_platform_test(TestSuite& suite) {
    auto protector = config::make_dpapi_secret_protector();
#ifdef _WIN32
    if (!protector) {
        MANNY_CHECK(suite, protector.error().code ==
                               config::SecretProtectionErrorCode::UnsupportedEnvironment);
        MANNY_CHECK(suite, !protector.error().system_error.has_value());
        return;
    }

    TempSecretTree tree;
    auto store_result =
        ProtectedFileSecretStore::create(tree.secret_directory(), std::move(*protector));
    MANNY_CHECK(suite, store_result.has_value());
    auto& store = *store_result;
    const auto plaintext = SecretValue::from_text("native-dpapi-round-trip-marker");
    MANNY_CHECK(suite, store.store(SecretId::TwitchOAuthSession, plaintext).has_value());
    const auto protected_path = store.record_path(SecretId::TwitchOAuthSession);
    auto protected_value = tree.read(protected_path);
    MANNY_CHECK(suite, !contains_bytes(protected_value, plaintext.bytes()));
    auto round_trip = store.load(SecretId::TwitchOAuthSession);
    MANNY_CHECK(suite, round_trip.has_value());
    MANNY_CHECK(suite, *round_trip == plaintext);

    protected_value[protected_value.size() / 2] ^= std::byte{0x01};
    tree.write(protected_path, protected_value);
    const auto tampered = store.load(SecretId::TwitchOAuthSession);
    MANNY_CHECK(suite, !tampered.has_value());
    MANNY_CHECK(suite, tampered.error().code == SecretStoreErrorCode::UnprotectionFailed ||
                           tampered.error().code == SecretStoreErrorCode::CorruptRecord);
#else
    MANNY_CHECK(suite, !protector.has_value());
    MANNY_CHECK(suite, protector.error().code ==
                           config::SecretProtectionErrorCode::UnsupportedEnvironment);
    MANNY_CHECK(suite, !protector.error().system_error.has_value());
#endif
}

} // namespace

void run_protected_secret_store_tests(TestSuite& suite) {
    secret_value_and_identifier_tests(suite);
    creation_and_missing_tests(suite);
    round_trip_and_replacement_tests(suite);
    validation_and_failure_tests(suite);
    corruption_and_substitution_tests(suite);
    atomic_failure_and_erase_tests(suite);
    invalid_directory_test(suite);
    dpapi_platform_test(suite);
}

} // namespace manny_uploader::test
