#include "manny_uploader/config/settings_store.hpp"
#include "support/test_suite.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace manny_uploader::test {
namespace {

using config::SettingsLoadSource;
using config::SettingsStore;
using config::SettingsStoreErrorCode;

class TempSettingsTree {
  public:
    TempSettingsTree() {
        static std::uint64_t sequence{};
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = std::filesystem::temp_directory_path() /
                ("manny-settings-" + std::to_string(timestamp) + "-" + std::to_string(++sequence));
        std::error_code error;
        if (!std::filesystem::create_directories(root_, error) || error) {
            throw std::runtime_error{"Could not create settings test directory"};
        }
    }

    ~TempSettingsTree() {
        std::error_code error;
        static_cast<void>(std::filesystem::remove_all(root_, error));
    }

    TempSettingsTree(const TempSettingsTree&) = delete;
    TempSettingsTree& operator=(const TempSettingsTree&) = delete;

    [[nodiscard]] std::filesystem::path settings_path() const {
        return root_ / "nested" / "settings.json";
    }

    void write(const std::filesystem::path& path, std::string_view contents) const {
        std::error_code error;
        static_cast<void>(std::filesystem::create_directories(path.parent_path(), error));
        if (error) {
            throw std::runtime_error{"Could not create settings fixture directory"};
        }
        std::ofstream stream{path, std::ios::binary | std::ios::trunc};
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!stream) {
            throw std::runtime_error{"Could not write settings fixture"};
        }
    }

    [[nodiscard]] std::string read(const std::filesystem::path& path) const {
        std::ifstream stream{path, std::ios::binary};
        return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    }

    void remove(const std::filesystem::path& path) const {
        std::error_code error;
        static_cast<void>(std::filesystem::remove(path, error));
        if (error) {
            throw std::runtime_error{"Could not remove settings fixture"};
        }
    }

  private:
    std::filesystem::path root_;
};

[[nodiscard]] config::Settings defaults() {
    return config::make_default_settings("C:/Users/Streamer/Documents/Guild Wars 2/logs");
}

[[nodiscard]] SettingsStore make_store(const std::filesystem::path& path) {
    auto store = SettingsStore::create(path, defaults());
    if (!store) {
        throw std::runtime_error{"Could not construct settings store fixture"};
    }
    return std::move(*store);
}

void creation_and_default_tests(TestSuite& suite) {
    auto invalid_path = SettingsStore::create({}, defaults());
    MANNY_CHECK(suite, !invalid_path.has_value());
    MANNY_CHECK(suite, invalid_path.error().code == SettingsStoreErrorCode::InvalidConfiguration);

    auto invalid_filename = SettingsStore::create(".", defaults());
    MANNY_CHECK(suite, !invalid_filename.has_value());
    MANNY_CHECK(suite,
                invalid_filename.error().code == SettingsStoreErrorCode::InvalidConfiguration);

    auto invalid_defaults = defaults();
    invalid_defaults.general.poll_interval_ms = 0;
    auto invalid = SettingsStore::create("settings.json", std::move(invalid_defaults));
    MANNY_CHECK(suite, !invalid.has_value());
    MANNY_CHECK(suite, invalid.error().code == SettingsStoreErrorCode::ValidationFailed);
    MANNY_CHECK(suite, !invalid.error().validation_errors.empty());

    TempSettingsTree tree;
    const auto path = tree.settings_path();
    const auto store = make_store(path);
    MANNY_CHECK(suite, store.primary_path() == path);
    MANNY_CHECK(suite, store.backup_path() == path.string() + ".bak");
    MANNY_CHECK(suite, store.temporary_path() == path.string() + ".tmp");

    const auto loaded = store.load();
    MANNY_CHECK(suite, loaded.has_value());
    MANNY_CHECK(suite, loaded->source == SettingsLoadSource::Defaults);
    MANNY_CHECK(suite, loaded->settings == defaults());
    MANNY_CHECK(suite, !loaded->recovery_diagnostic.has_value());
}

void round_trip_tests(TestSuite& suite) {
    TempSettingsTree tree;
    const auto store = make_store(tree.settings_path());
    auto settings = defaults();
    settings.general.watch_subdirectories = false;
    settings.general.poll_interval_ms = 250;
    settings.general.stability_observations = 10;
    settings.general.recent_log_limit = 500;
    settings.general.parser_queue_capacity = 64;
    settings.general.parallel_uploads_per_provider = 10;
    settings.general.max_candidates = 10'000;
    settings.dps_report.enabled = true;
    settings.wingman.enabled = false;
    settings.donbot.enabled = true;
    settings.donbot.api_base_url = "https://donbot.example/v1/";
    settings.donbot.selected_guild_id = "123456789012345678";
    settings.donbot.discord_delivery_enabled = true;
    settings.donbot.selected_discord_channel_id = "223456789012345678";
    settings.twitch.enabled = true;
    settings.twitch.client_id = "abc123publicclient";
    settings.twitch.message_template = "{encounter}: {url} — Uploaded";
    settings.twitch.post_failure = false;

    const auto saved = store.save(settings);
    MANNY_CHECK(suite, saved.has_value());
    MANNY_CHECK(suite, std::filesystem::exists(store.primary_path()));
    MANNY_CHECK(suite, !std::filesystem::exists(store.backup_path()));
    MANNY_CHECK(suite, !std::filesystem::exists(store.temporary_path()));
    MANNY_CHECK(suite, !std::filesystem::exists(store.backup_path().string() + ".tmp"));

    const auto document = tree.read(store.primary_path());
    MANNY_CHECK(suite, document.ends_with('\n'));
    MANNY_CHECK(suite, document.find("\n  \"general\"") != std::string::npos);
    MANNY_CHECK(suite, document.find("\"schema_version\": 1") != std::string::npos);
    MANNY_CHECK(suite, document.find("— Uploaded") != std::string::npos);
    MANNY_CHECK(suite, document.find("access_token") == std::string::npos);
    MANNY_CHECK(suite, document.find("refresh_token") == std::string::npos);
    MANNY_CHECK(suite, document.find("api_key") == std::string::npos);

    const auto loaded = store.load();
    MANNY_CHECK(suite, loaded.has_value());
    MANNY_CHECK(suite, loaded->source == SettingsLoadSource::Primary);
    MANNY_CHECK(suite, loaded->settings == settings);
    MANNY_CHECK(suite, !loaded->recovery_diagnostic.has_value());
}

void partial_and_strict_json_tests(TestSuite& suite) {
    TempSettingsTree tree;
    const auto path = tree.settings_path();
    const auto store = make_store(path);

    tree.write(path, R"({"schema_version":1,"general":{"log_directory":"D:/Guild Wars 2/logs"}})");
    auto loaded = store.load();
    MANNY_CHECK(suite, loaded.has_value());
    MANNY_CHECK(suite, loaded->settings.general.log_directory == "D:/Guild Wars 2/logs");
    MANNY_CHECK(suite, loaded->settings.general.poll_interval_ms == 1000);
    MANNY_CHECK(suite, loaded->settings.dps_report.enabled);
    MANNY_CHECK(suite, loaded->settings.wingman.enabled);
    MANNY_CHECK(suite, !loaded->settings.donbot.discord_delivery_enabled);
    MANNY_CHECK(suite, loaded->settings.donbot.selected_discord_channel_id.empty());
    MANNY_CHECK(suite, !loaded->settings.twitch.enabled);

    const std::vector<std::string> invalid_documents{
        R"({"general":{"log_directory":"D:/logs"}})",
        R"({"schema_version":2,"general":{"log_directory":"D:/logs"}})",
        R"({"schema_version":1,"general":{"log_directory":42}})",
        R"({"schema_version":1,"general":{"log_directory":"D:/logs","api_key":"secret-value"}})",
        R"({"schema_version":1,"general":{"log_directory":"D:/logs"}} trailing)",
        R"({"schema_version":1,)",
    };
    for (const auto& document : invalid_documents) {
        tree.write(path, document);
        loaded = store.load();
        MANNY_CHECK(suite, !loaded.has_value());
        MANNY_CHECK(suite, loaded.error().code == SettingsStoreErrorCode::NoValidSettings);
    }

    std::string invalid_utf8 = R"({"schema_version":1,"general":{"log_directory":"D:/logs/)";
    invalid_utf8.push_back(static_cast<char>(0xc0));
    invalid_utf8.push_back(static_cast<char>(0x80));
    invalid_utf8 += R"("}})";
    tree.write(path, invalid_utf8);
    loaded = store.load();
    MANNY_CHECK(suite, !loaded.has_value());
    MANNY_CHECK(suite, loaded.error().code == SettingsStoreErrorCode::NoValidSettings);

    tree.write(path, std::string(config::max_settings_file_bytes + 1, ' '));
    loaded = store.load();
    MANNY_CHECK(suite, !loaded.has_value());
    MANNY_CHECK(suite, loaded.error().code == SettingsStoreErrorCode::NoValidSettings);
}

void backup_recovery_tests(TestSuite& suite) {
    TempSettingsTree tree;
    const auto store = make_store(tree.settings_path());
    auto first = defaults();
    first.general.recent_log_limit = 10;
    auto second = defaults();
    second.general.recent_log_limit = 20;
    auto third = defaults();
    third.general.recent_log_limit = 30;

    MANNY_CHECK(suite, store.save(first).has_value());
    MANNY_CHECK(suite, store.save(second).has_value());
    MANNY_CHECK(suite, std::filesystem::exists(store.backup_path()));

    tree.write(store.primary_path(), "not json");
    auto loaded = store.load();
    MANNY_CHECK(suite, loaded.has_value());
    MANNY_CHECK(suite, loaded->source == SettingsLoadSource::Backup);
    MANNY_CHECK(suite, loaded->settings == first);
    MANNY_CHECK(suite, loaded->recovery_diagnostic.has_value());

    MANNY_CHECK(suite, store.save(third).has_value());
    tree.write(store.primary_path(), "still not json");
    loaded = store.load();
    MANNY_CHECK(suite, loaded.has_value());
    MANNY_CHECK(suite, loaded->source == SettingsLoadSource::Backup);
    MANNY_CHECK(suite, loaded->settings == first);

    tree.write(store.backup_path(), "also not json");
    loaded = store.load();
    MANNY_CHECK(suite, !loaded.has_value());
    MANNY_CHECK(suite, loaded.error().code == SettingsStoreErrorCode::NoValidSettings);
}

void missing_primary_and_failed_save_tests(TestSuite& suite) {
    TempSettingsTree tree;
    const auto store = make_store(tree.settings_path());
    auto first = defaults();
    first.general.recent_log_limit = 11;
    auto second = defaults();
    second.general.recent_log_limit = 22;
    MANNY_CHECK(suite, store.save(first).has_value());
    MANNY_CHECK(suite, store.save(second).has_value());

    tree.remove(store.primary_path());
    auto loaded = store.load();
    MANNY_CHECK(suite, loaded.has_value());
    MANNY_CHECK(suite, loaded->source == SettingsLoadSource::Backup);
    MANNY_CHECK(suite, loaded->settings == first);
    MANNY_CHECK(suite, loaded->recovery_diagnostic.has_value());

    MANNY_CHECK(suite, store.save(second).has_value());
    auto invalid = second;
    invalid.twitch.enabled = true;
    invalid.dps_report.enabled = false;
    const auto rejected = store.save(invalid);
    MANNY_CHECK(suite, !rejected.has_value());
    MANNY_CHECK(suite, rejected.error().code == SettingsStoreErrorCode::ValidationFailed);
    MANNY_CHECK(suite, !rejected.error().validation_errors.empty());

    loaded = store.load();
    MANNY_CHECK(suite, loaded.has_value());
    MANNY_CHECK(suite, loaded->source == SettingsLoadSource::Primary);
    MANNY_CHECK(suite, loaded->settings == second);
}

} // namespace

void run_settings_store_tests(TestSuite& suite) {
    creation_and_default_tests(suite);
    round_trip_tests(suite);
    partial_and_strict_json_tests(suite);
    backup_recovery_tests(suite);
    missing_primary_and_failed_save_tests(suite);
}

} // namespace manny_uploader::test
