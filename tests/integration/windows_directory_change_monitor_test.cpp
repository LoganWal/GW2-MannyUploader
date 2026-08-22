#include "manny_uploader/filesystem/change_notifying_log_candidate_source.hpp"
#include "support/test_suite.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

namespace manny_uploader::test {
namespace {

using filesystem::DirectoryChangeStatus;
using filesystem::IDirectoryChangeMonitor;
using ports::LogCandidateSourceError;
using ports::LogCandidateSourceErrorCode;

class TemporaryDirectory {
  public:
    TemporaryDirectory() {
        static std::atomic_uint64_t next_id{};
        base_ = std::filesystem::temp_directory_path() /
                ("manny-uploader-native-monitor-test-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                 std::to_string(next_id.fetch_add(1)));
        root_ = base_ / "logs";
        if (!std::filesystem::create_directories(base_)) {
            throw std::runtime_error{"Unable to create native-monitor test directory"};
        }
    }

    ~TemporaryDirectory() {
        std::error_code error;
        static_cast<void>(std::filesystem::remove_all(base_, error));
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    void create_root() const {
        std::error_code error;
        static_cast<void>(std::filesystem::create_directories(root_, error));
        if (error) {
            throw std::runtime_error{"Unable to create native-monitor log root"};
        }
    }

    void write(std::string_view contents, bool append = false) const {
        const auto mode = std::ios::binary | (append ? std::ios::app : std::ios::trunc);
        std::ofstream stream{root_ / "encounter.zevtc", mode};
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!stream) {
            throw std::runtime_error{"Unable to write native-monitor fixture"};
        }
    }

    [[nodiscard]] const std::filesystem::path& root() const noexcept {
        return root_;
    }

  private:
    std::filesystem::path base_;
    std::filesystem::path root_;
};

[[nodiscard]] std::expected<DirectoryChangeStatus, LogCandidateSourceError>
wait_for_status(IDirectoryChangeMonitor& monitor, DirectoryChangeStatus wanted) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    auto result = monitor.poll({});
    while (result && *result != wanted && *result != DirectoryChangeStatus::Unavailable &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
        result = monitor.poll({});
    }
    return result;
}

void native_monitor_contract_tests(TestSuite& suite) {
    TemporaryDirectory observed;
    TemporaryDirectory initially_missing;
    observed.create_root();

    auto monitor = filesystem::make_windows_directory_change_monitor();
    MANNY_CHECK(suite, monitor != nullptr);
    if (!monitor) {
        return;
    }

    const auto invalid = monitor->reconfigure({}, true);
    MANNY_CHECK(suite, !invalid.has_value());
    if (invalid) {
        return;
    }
    MANNY_CHECK(suite, invalid.error().code == LogCandidateSourceErrorCode::InvalidConfiguration);

    const auto configured = monitor->reconfigure(observed.root(), true);
    MANNY_CHECK(suite, configured.has_value());
    if (!configured) {
        return;
    }

    const auto initial = monitor->poll({});
    MANNY_CHECK(suite, initial.has_value());
    MANNY_CHECK(suite, initial && *initial == DirectoryChangeStatus::Changed);

    const auto idle = monitor->poll({});
    MANNY_CHECK(suite, idle.has_value());
    MANNY_CHECK(suite, idle && *idle == DirectoryChangeStatus::Unchanged);

    observed.write("created");
    const auto created = wait_for_status(*monitor, DirectoryChangeStatus::Changed);
    MANNY_CHECK(suite, created.has_value());
    MANNY_CHECK(suite, created && *created == DirectoryChangeStatus::Changed);

    const auto quiet = wait_for_status(*monitor, DirectoryChangeStatus::Unchanged);
    MANNY_CHECK(suite, quiet.has_value());
    MANNY_CHECK(suite, quiet && *quiet == DirectoryChangeStatus::Unchanged);

    observed.write("-expanded", true);
    const auto modified = wait_for_status(*monitor, DirectoryChangeStatus::Changed);
    MANNY_CHECK(suite, modified.has_value());
    MANNY_CHECK(suite, modified && *modified == DirectoryChangeStatus::Changed);

    const auto missing_configured = monitor->reconfigure(initially_missing.root(), false);
    MANNY_CHECK(suite, missing_configured.has_value());
    const auto missing = monitor->poll({});
    MANNY_CHECK(suite, missing.has_value());
    MANNY_CHECK(suite, missing && *missing == DirectoryChangeStatus::Unavailable);

    initially_missing.create_root();
    const auto appeared = monitor->poll({});
    MANNY_CHECK(suite, appeared.has_value());
    MANNY_CHECK(suite, appeared && *appeared == DirectoryChangeStatus::Changed);

    std::stop_source stop;
    stop.request_stop();
    const auto cancelled = monitor->poll(stop.get_token());
    MANNY_CHECK(suite, !cancelled.has_value());
    if (cancelled) {
        return;
    }
    MANNY_CHECK(suite, cancelled.error().code == LogCandidateSourceErrorCode::Cancelled);
}

} // namespace

void run_windows_directory_change_monitor_tests(TestSuite& suite) {
    native_monitor_contract_tests(suite);
}

} // namespace manny_uploader::test
