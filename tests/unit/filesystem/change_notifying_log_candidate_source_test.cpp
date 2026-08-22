#include "manny_uploader/filesystem/change_notifying_log_candidate_source.hpp"
#include "support/test_suite.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace manny_uploader::test {
namespace {

using filesystem::ChangeNotifyingLogCandidateSource;
using filesystem::DirectoryChangeStatus;
using filesystem::IDirectoryChangeMonitor;
using ports::LogCandidateSourceError;
using ports::LogCandidateSourceErrorCode;

[[nodiscard]] LogCandidateSourceError monitor_failure() {
    return LogCandidateSourceError{
        .code = LogCandidateSourceErrorCode::ScanFailed,
        .message = "native notification fixture failure",
    };
}

class FakeChangeMonitor final : public IDirectoryChangeMonitor {
  public:
    [[nodiscard]] std::expected<void, LogCandidateSourceError>
    reconfigure(const std::filesystem::path& root, bool recursive) override {
        ++reconfigurations;
        configured_root = root;
        configured_recursive = recursive;
        if (fail_reconfigure) {
            return std::unexpected(monitor_failure());
        }
        return {};
    }

    [[nodiscard]] std::expected<DirectoryChangeStatus, LogCandidateSourceError>
    poll(const std::stop_token& stop_token) override {
        ++polls;
        if (stop_token.stop_requested()) {
            return std::unexpected(LogCandidateSourceError{
                .code = LogCandidateSourceErrorCode::Cancelled,
                .message = "cancelled",
            });
        }
        if (results.empty()) {
            return DirectoryChangeStatus::Unchanged;
        }
        auto result = std::move(results.front());
        results.pop_front();
        return result;
    }

    void push(DirectoryChangeStatus status) {
        results.emplace_back(status);
    }

    void push_failure() {
        results.emplace_back(std::unexpected(monitor_failure()));
    }

    std::deque<std::expected<DirectoryChangeStatus, LogCandidateSourceError>> results;
    std::filesystem::path configured_root;
    std::size_t reconfigurations{};
    std::size_t polls{};
    bool configured_recursive{};
    bool fail_reconfigure{};
};

class TempTree {
  public:
    TempTree() {
        static std::atomic_uint64_t next_id{};
        base_ = std::filesystem::temp_directory_path() /
                ("manny-uploader-change-source-test-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                 std::to_string(next_id.fetch_add(1)));
        root_ = base_ / "logs";
        if (!std::filesystem::create_directories(base_)) {
            throw std::runtime_error{"Unable to create notification-source test directory"};
        }
    }

    ~TempTree() {
        std::error_code error;
        static_cast<void>(std::filesystem::remove_all(base_, error));
    }

    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;

    void create_root() const {
        std::error_code error;
        static_cast<void>(std::filesystem::create_directories(root_, error));
        if (error) {
            throw std::runtime_error{"Unable to create notification-source log root"};
        }
    }

    void write(std::string_view contents) const {
        std::ofstream stream{root_ / "encounter.zevtc", std::ios::binary | std::ios::trunc};
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!stream) {
            throw std::runtime_error{"Unable to write notification-source fixture"};
        }
    }

    void remove_log() const {
        std::error_code error;
        static_cast<void>(std::filesystem::remove(root_ / "encounter.zevtc", error));
        if (error) {
            throw std::runtime_error{"Unable to remove notification-source fixture"};
        }
    }

    [[nodiscard]] std::filesystem::path canonical_log() const {
        return std::filesystem::weakly_canonical(root_ / "encounter.zevtc");
    }

    [[nodiscard]] const std::filesystem::path& root() const noexcept {
        return root_;
    }

  private:
    std::filesystem::path base_;
    std::filesystem::path root_;
};

void configuration_and_initialization_tests(TestSuite& suite) {
    auto monitor = std::make_unique<FakeChangeMonitor>();
    const auto invalid = ChangeNotifyingLogCandidateSource::create({}, true, 8, std::move(monitor));
    MANNY_CHECK(suite, !invalid.has_value());
    MANNY_CHECK(suite, invalid.error().code == LogCandidateSourceErrorCode::InvalidConfiguration);

    const auto no_failure_capacity =
        ChangeNotifyingLogCandidateSource::create("logs", true, 8, nullptr, 0);
    MANNY_CHECK(suite, !no_failure_capacity.has_value());

    TempTree tree;
    tree.create_root();
    tree.write("initial");
    auto valid_monitor = std::make_unique<FakeChangeMonitor>();
    auto* observer = valid_monitor.get();
    auto created =
        ChangeNotifyingLogCandidateSource::create(tree.root(), true, 8, std::move(valid_monitor));
    MANNY_CHECK(suite, created.has_value());
    MANNY_CHECK(suite, created->using_native_notifications());
    MANNY_CHECK(suite, created->root().is_absolute());
    MANNY_CHECK(suite, created->recursive());
    MANNY_CHECK(suite, created->max_candidates() == 8);
    MANNY_CHECK(suite, observer->reconfigurations == 1);
    MANNY_CHECK(suite, observer->configured_root == created->root());

    auto failed_monitor = std::make_unique<FakeChangeMonitor>();
    failed_monitor->fail_reconfigure = true;
    auto fallback =
        ChangeNotifyingLogCandidateSource::create(tree.root(), true, 8, std::move(failed_monitor));
    MANNY_CHECK(suite, fallback.has_value());
    MANNY_CHECK(suite, !fallback->using_native_notifications());
    const auto fallback_batch = fallback->poll({});
    MANNY_CHECK(suite, fallback_batch.has_value());
    MANNY_CHECK(suite, fallback_batch->issues.size() == 1);
}

void cached_observation_and_removal_tests(TestSuite& suite) {
    TempTree tree;
    tree.create_root();
    tree.write("one");
    const auto canonical_log = tree.canonical_log();

    auto monitor = std::make_unique<FakeChangeMonitor>();
    auto* observer = monitor.get();
    auto created =
        ChangeNotifyingLogCandidateSource::create(tree.root(), true, 8, std::move(monitor));
    MANNY_CHECK(suite, created.has_value());
    auto source = std::move(*created);

    const auto initial = source.poll({});
    MANNY_CHECK(suite, initial.has_value());
    MANNY_CHECK(suite, initial->observations.size() == 1);
    MANNY_CHECK(suite, initial->observations.front().size == 3);
    MANNY_CHECK(suite, observer->polls == 0);

    tree.write("expanded");
    observer->push(DirectoryChangeStatus::Unchanged);
    const auto cached = source.poll({});
    MANNY_CHECK(suite, cached.has_value());
    MANNY_CHECK(suite, cached->observations.front().size == 3);

    observer->push(DirectoryChangeStatus::Changed);
    const auto changed = source.poll({});
    MANNY_CHECK(suite, changed.has_value());
    MANNY_CHECK(suite, changed->observations.front().size == 8);

    tree.remove_log();
    observer->push(DirectoryChangeStatus::Changed);
    const auto removed = source.poll({});
    MANNY_CHECK(suite, removed.has_value());
    MANNY_CHECK(suite, removed->removed_paths.size() == 1);
    MANNY_CHECK(suite, removed->removed_paths.front() == canonical_log);

    observer->push(DirectoryChangeStatus::Unchanged);
    const auto repeated = source.poll({});
    MANNY_CHECK(suite, repeated.has_value());
    MANNY_CHECK(suite, repeated->removed_paths.empty());
    MANNY_CHECK(suite, repeated->observations.empty());
}

void missing_root_and_failure_fallback_tests(TestSuite& suite) {
    TempTree missing_tree;
    auto missing_monitor = std::make_unique<FakeChangeMonitor>();
    auto* missing_observer = missing_monitor.get();
    auto missing_created = ChangeNotifyingLogCandidateSource::create(missing_tree.root(), true, 8,
                                                                     std::move(missing_monitor));
    MANNY_CHECK(suite, missing_created.has_value());
    auto missing_source = std::move(*missing_created);

    const auto missing = missing_source.poll({});
    MANNY_CHECK(suite, missing.has_value());
    MANNY_CHECK(suite, !missing->root_available);
    missing_tree.create_root();
    missing_tree.write("created");
    const auto discovered = missing_source.poll({});
    MANNY_CHECK(suite, discovered.has_value());
    MANNY_CHECK(suite, discovered->root_available);
    MANNY_CHECK(suite, discovered->observations.size() == 1);
    MANNY_CHECK(suite, missing_observer->polls == 0);
    missing_tree.write("created and expanded");
    missing_observer->push(DirectoryChangeStatus::Unavailable);
    const auto unavailable_rescan = missing_source.poll({});
    MANNY_CHECK(suite, unavailable_rescan.has_value());
    MANNY_CHECK(suite, unavailable_rescan->observations.front().size == 20);

    TempTree tree;
    tree.create_root();
    tree.write("one");
    auto monitor = std::make_unique<FakeChangeMonitor>();
    auto* observer = monitor.get();
    auto created =
        ChangeNotifyingLogCandidateSource::create(tree.root(), true, 8, std::move(monitor), 2);
    MANNY_CHECK(suite, created.has_value());
    auto source = std::move(*created);
    MANNY_CHECK(suite, source.poll({}).has_value());

    tree.write("first failure");
    observer->push_failure();
    const auto first_failure = source.poll({});
    MANNY_CHECK(suite, first_failure.has_value());
    MANNY_CHECK(suite, first_failure->observations.front().size == 13);
    MANNY_CHECK(suite, first_failure->issues.size() == 1);
    MANNY_CHECK(suite, source.using_native_notifications());
    MANNY_CHECK(suite, source.consecutive_monitor_failures() == 1);

    tree.write("second failure");
    observer->push_failure();
    const auto second_failure = source.poll({});
    MANNY_CHECK(suite, second_failure.has_value());
    MANNY_CHECK(suite, second_failure->observations.front().size == 14);
    MANNY_CHECK(suite, second_failure->issues.size() == 1);
    MANNY_CHECK(suite, !source.using_native_notifications());
    MANNY_CHECK(suite, source.consecutive_monitor_failures() == 2);

    const auto monitor_polls = observer->polls;
    tree.write("polling fallback");
    observer->push(DirectoryChangeStatus::Unchanged);
    const auto fallback = source.poll({});
    MANNY_CHECK(suite, fallback.has_value());
    MANNY_CHECK(suite, fallback->observations.front().size == 16);
    MANNY_CHECK(suite, observer->polls == monitor_polls);
}

void reconfiguration_and_cancellation_tests(TestSuite& suite) {
    TempTree first;
    first.create_root();
    first.write("first");
    TempTree second;
    second.create_root();
    second.write("replacement");

    auto monitor = std::make_unique<FakeChangeMonitor>();
    auto* observer = monitor.get();
    auto created =
        ChangeNotifyingLogCandidateSource::create(first.root(), true, 8, std::move(monitor));
    MANNY_CHECK(suite, created.has_value());
    auto source = std::move(*created);
    MANNY_CHECK(suite, source.poll({}).has_value());

    MANNY_CHECK(suite, source.reconfigure(second.root(), false, 4).has_value());
    MANNY_CHECK(suite, source.using_native_notifications());
    MANNY_CHECK(suite, observer->reconfigurations == 2);
    MANNY_CHECK(suite, observer->configured_root == std::filesystem::absolute(second.root()));
    MANNY_CHECK(suite, !observer->configured_recursive);
    const auto replacement = source.poll({});
    MANNY_CHECK(suite, replacement.has_value());
    MANNY_CHECK(suite, replacement->observations.front().size == 11);

    const auto old_root = source.root();
    const auto invalid = source.reconfigure({}, true, 8);
    MANNY_CHECK(suite, !invalid.has_value());
    MANNY_CHECK(suite, source.root() == old_root);

    std::stop_source stop;
    stop.request_stop();
    const auto cancelled = source.poll(stop.get_token());
    MANNY_CHECK(suite, !cancelled.has_value());
    MANNY_CHECK(suite, cancelled.error().code == LogCandidateSourceErrorCode::Cancelled);
}

} // namespace

void run_change_notifying_log_candidate_source_tests(TestSuite& suite) {
    configuration_and_initialization_tests(suite);
    cached_observation_and_removal_tests(suite);
    missing_root_and_failure_fallback_tests(suite);
    reconfiguration_and_cancellation_tests(suite);
}

} // namespace manny_uploader::test
