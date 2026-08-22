#include "manny_uploader/filesystem/polling_log_candidate_source.hpp"
#include "support/test_suite.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace manny_uploader::test {
namespace {

using filesystem::CandidateSnapshotTracker;
using filesystem::DirectorySnapshot;
using filesystem::StandardPollingLogCandidateSource;
using ports::LogCandidateSourceErrorCode;

class TempTree {
  public:
    TempTree() {
        static std::atomic_uint64_t next_id{};
        base_ = std::filesystem::temp_directory_path() /
                ("manny-uploader-poll-test-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                 std::to_string(next_id.fetch_add(1)));
        root_ = base_ / "logs";
        if (!std::filesystem::create_directories(base_)) {
            throw std::runtime_error{"Unable to create polling test directory"};
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
            throw std::runtime_error{"Unable to create log root fixture"};
        }
    }

    void write(const std::filesystem::path& relative_path, std::string_view contents) const {
        const auto destination = root_ / relative_path;
        std::error_code error;
        static_cast<void>(std::filesystem::create_directories(destination.parent_path(), error));
        if (error) {
            throw std::runtime_error{"Unable to create polling fixture parent"};
        }
        std::ofstream stream{destination, std::ios::binary | std::ios::trunc};
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!stream) {
            throw std::runtime_error{"Unable to write polling fixture"};
        }
    }

    void remove(const std::filesystem::path& relative_path) const {
        std::error_code error;
        static_cast<void>(std::filesystem::remove(root_ / relative_path, error));
        if (error) {
            throw std::runtime_error{"Unable to remove polling fixture"};
        }
    }

    void set_last_write_time(const std::filesystem::path& relative_path,
                             std::filesystem::file_time_type value) const {
        std::error_code error;
        std::filesystem::last_write_time(root_ / relative_path, value, error);
        if (error) {
            throw std::runtime_error{"Unable to timestamp polling fixture"};
        }
    }

    void remove_root() const {
        std::error_code error;
        static_cast<void>(std::filesystem::remove_all(root_, error));
        if (error) {
            throw std::runtime_error{"Unable to remove log root fixture"};
        }
    }

    [[nodiscard]] std::filesystem::path canonical(const std::filesystem::path& relative) const {
        return std::filesystem::weakly_canonical(root_ / relative);
    }

    [[nodiscard]] const std::filesystem::path& base() const noexcept {
        return base_;
    }

    [[nodiscard]] const std::filesystem::path& root() const noexcept {
        return root_;
    }

  private:
    std::filesystem::path base_;
    std::filesystem::path root_;
};

void configuration_tests(TestSuite& suite) {
    const auto empty = StandardPollingLogCandidateSource::create({});
    MANNY_CHECK(suite, !empty.has_value());
    MANNY_CHECK(suite, empty.error().code == LogCandidateSourceErrorCode::InvalidConfiguration);

    const auto no_capacity = StandardPollingLogCandidateSource::create("logs", true, 0);
    MANNY_CHECK(suite, !no_capacity.has_value());
    MANNY_CHECK(suite,
                no_capacity.error().code == LogCandidateSourceErrorCode::InvalidConfiguration);
}

void missing_creation_and_recursive_tests(TestSuite& suite) {
    TempTree tree;
    auto created = StandardPollingLogCandidateSource::create(tree.root(), true, 8);
    MANNY_CHECK(suite, created.has_value());
    auto source = std::move(created.value());
    MANNY_CHECK(suite, source.recursive());
    MANNY_CHECK(suite, source.max_candidates() == 8);
    MANNY_CHECK(suite, source.root().is_absolute());

    const auto missing = source.poll({});
    MANNY_CHECK(suite, missing.has_value());
    MANNY_CHECK(suite, !missing->root_available);
    MANNY_CHECK(suite, missing->scan_complete);
    MANNY_CHECK(suite, missing->observations.empty());

    const std::filesystem::path unicode_path{u8"nested/ström.ZEVTC"};
    tree.create_root();
    tree.write("first.zevtc", "one");
    tree.write(unicode_path, "unicode");
    tree.write("ignored.evtc", "raw");
    tree.write("notes.txt", "ignored");
    static_cast<void>(std::filesystem::create_directory(tree.root() / "directory.zevtc"));

    const auto discovered = source.poll({});
    MANNY_CHECK(suite, discovered.has_value());
    MANNY_CHECK(suite, discovered->root_available);
    MANNY_CHECK(suite, discovered->scan_complete);
    MANNY_CHECK(suite, discovered->issues.empty());
    MANNY_CHECK(suite, discovered->observations.size() == 2);
    MANNY_CHECK(suite, discovered->removed_paths.empty());
    MANNY_CHECK(suite, discovered->observations[0].canonical_path == tree.canonical("first.zevtc"));
    MANNY_CHECK(suite, discovered->observations[0].size == 3);
    MANNY_CHECK(suite, discovered->observations[1].canonical_path == tree.canonical(unicode_path));
    MANNY_CHECK(suite, discovered->observations[1].size == 7);
    MANNY_CHECK(suite, source.retained_count() == 2);

    const auto unchanged = source.poll({});
    MANNY_CHECK(suite, unchanged.has_value());
    MANNY_CHECK(suite, unchanged->removed_paths.empty());

    const auto removed_path = tree.canonical("first.zevtc");
    tree.remove("first.zevtc");
    const auto removed = source.poll({});
    MANNY_CHECK(suite, removed.has_value());
    MANNY_CHECK(suite, removed->removed_paths.size() == 1);
    MANNY_CHECK(suite, removed->removed_paths.front() == removed_path);
    MANNY_CHECK(suite, source.retained_count() == 1);

    const auto remaining_path = tree.canonical(unicode_path);
    tree.remove_root();
    const auto missing_again = source.poll({});
    MANNY_CHECK(suite, missing_again.has_value());
    MANNY_CHECK(suite, !missing_again->root_available);
    MANNY_CHECK(suite, missing_again->removed_paths.size() == 1);
    MANNY_CHECK(suite, missing_again->removed_paths.front() == remaining_path);
    MANNY_CHECK(suite, source.retained_count() == 0);

    tree.create_root();
    tree.write("later.zevtc", "new");
    const auto created_later = source.poll({});
    MANNY_CHECK(suite, created_later.has_value());
    MANNY_CHECK(suite, created_later->root_available);
    MANNY_CHECK(suite, created_later->observations.size() == 1);
    MANNY_CHECK(suite, created_later->observations.front().canonical_path ==
                           tree.canonical("later.zevtc"));
}

void non_recursive_and_root_validation_tests(TestSuite& suite) {
    TempTree tree;
    tree.create_root();
    tree.write("top.zevtc", "top");
    tree.write("nested/deep.zevtc", "deep");

    auto created = StandardPollingLogCandidateSource::create(tree.root(), false, 8);
    MANNY_CHECK(suite, created.has_value());
    auto source = std::move(created.value());
    MANNY_CHECK(suite, !source.recursive());
    const auto batch = source.poll({});
    MANNY_CHECK(suite, batch.has_value());
    MANNY_CHECK(suite, batch->observations.size() == 1);
    MANNY_CHECK(suite, batch->observations.front().canonical_path == tree.canonical("top.zevtc"));

    tree.write("not-a-directory", "file");
    auto file_root = StandardPollingLogCandidateSource::create(tree.root() / "not-a-directory");
    MANNY_CHECK(suite, file_root.has_value());
    const auto invalid = file_root->poll({});
    MANNY_CHECK(suite, !invalid.has_value());
    MANNY_CHECK(suite, invalid.error().code == LogCandidateSourceErrorCode::ScanFailed);
}

void live_reconfiguration_tests(TestSuite& suite) {
    TempTree first;
    first.create_root();
    first.write("top.zevtc", "first");
    first.write("nested/deep.zevtc", "nested");
    TempTree second;
    second.create_root();
    second.write("replacement.zevtc", "second");
    second.write("nested/ignored.zevtc", "nested");

    auto created = StandardPollingLogCandidateSource::create(first.root(), true, 8);
    MANNY_CHECK(suite, created.has_value());
    auto source = std::move(*created);
    const auto initial = source.poll({});
    MANNY_CHECK(suite, initial.has_value() && initial->observations.size() == 2);
    MANNY_CHECK(suite, source.retained_count() == 2);

    const auto old_root = source.root();
    const auto invalid_path = source.reconfigure({}, false, 1);
    MANNY_CHECK(suite, !invalid_path.has_value());
    MANNY_CHECK(suite, source.root() == old_root);
    MANNY_CHECK(suite, source.recursive());
    MANNY_CHECK(suite, source.max_candidates() == 8);
    MANNY_CHECK(suite, source.retained_count() == 2);

    const auto invalid_limit = source.reconfigure(second.root(), false, 0);
    MANNY_CHECK(suite, !invalid_limit.has_value());
    MANNY_CHECK(suite, source.root() == old_root);

    MANNY_CHECK(suite, source.reconfigure(second.root(), false, 1).has_value());
    MANNY_CHECK(suite, source.root() == std::filesystem::absolute(second.root()));
    MANNY_CHECK(suite, !source.recursive());
    MANNY_CHECK(suite, source.max_candidates() == 1);
    MANNY_CHECK(suite, source.retained_count() == 0);

    const auto reconfigured = source.poll({});
    MANNY_CHECK(suite, reconfigured.has_value());
    MANNY_CHECK(suite, reconfigured->observations.size() == 1);
    MANNY_CHECK(suite, reconfigured->observations.front().canonical_path ==
                           second.canonical("replacement.zevtc"));
}

void resource_limit_and_cancellation_tests(TestSuite& suite) {
    TempTree tree;
    tree.create_root();
    tree.write("one.zevtc", "1");
    tree.write("two.zevtc", "2");

    auto created = StandardPollingLogCandidateSource::create(tree.root(), true, 1);
    MANNY_CHECK(suite, created.has_value());
    auto source = std::move(created.value());
    const auto limited = source.poll({});
    MANNY_CHECK(suite, !limited.has_value());
    MANNY_CHECK(suite, limited.error().code == LogCandidateSourceErrorCode::ResourceLimit);
    MANNY_CHECK(suite, source.retained_count() == 0);

    tree.remove("two.zevtc");
    const auto recovered = source.poll({});
    MANNY_CHECK(suite, recovered.has_value());
    MANNY_CHECK(suite, recovered->observations.size() == 1);

    std::stop_source stop_source;
    stop_source.request_stop();
    const auto cancelled = source.poll(stop_source.get_token());
    MANNY_CHECK(suite, !cancelled.has_value());
    MANNY_CHECK(suite, cancelled.error().code == LogCandidateSourceErrorCode::Cancelled);
    MANNY_CHECK(suite, source.retained_count() == 1);
}

void cutoff_and_large_archive_tests(TestSuite& suite) {
    TempTree tree;
    tree.create_root();
    const auto cutoff = std::filesystem::file_time_type::clock::now();
    for (std::size_t index = 0; index < 12; ++index) {
        const auto name = "old-" + std::to_string(index) + ".zevtc";
        tree.write(name, "old");
        tree.set_last_write_time(name, cutoff - std::chrono::hours{2});
    }
    tree.write("new-one.zevtc", "one");
    tree.write("new-two.zevtc", "two");
    tree.set_last_write_time("new-one.zevtc", cutoff + std::chrono::seconds{1});
    tree.set_last_write_time("new-two.zevtc", cutoff + std::chrono::seconds{2});

    auto created = StandardPollingLogCandidateSource::create(tree.root(), true, 2, cutoff);
    MANNY_CHECK(suite, created.has_value());
    auto source = std::move(*created);
    MANNY_CHECK(suite, source.minimum_last_write_time() == cutoff);
    const auto filtered = source.poll({});
    MANNY_CHECK(suite, filtered.has_value());
    MANNY_CHECK(suite, filtered->observations.size() == 2);
    MANNY_CHECK(suite,
                filtered->observations.front().canonical_path == tree.canonical("new-one.zevtc"));
    MANNY_CHECK(suite,
                filtered->observations.back().canonical_path == tree.canonical("new-two.zevtc"));

    tree.write("new-three.zevtc", "three");
    tree.set_last_write_time("new-three.zevtc", cutoff + std::chrono::seconds{3});
    const auto eligible_limit = source.poll({});
    MANNY_CHECK(suite, !eligible_limit.has_value());
    MANNY_CHECK(suite, eligible_limit.error().code == LogCandidateSourceErrorCode::ResourceLimit);

    MANNY_CHECK(
        suite,
        source.reconfigure(tree.root(), true, 2, cutoff + std::chrono::hours{1}).has_value());
    const auto none = source.poll({});
    MANNY_CHECK(suite, none.has_value());
    MANNY_CHECK(suite, none->observations.empty());

    MANNY_CHECK(suite, source.reconfigure(tree.root(), true, 2).has_value());
    const auto unfiltered = source.poll({});
    MANNY_CHECK(suite, !unfiltered.has_value());
    MANNY_CHECK(suite, unfiltered.error().code == LogCandidateSourceErrorCode::ResourceLimit);
}

void incomplete_snapshot_reconciliation_tests(TestSuite& suite) {
    const auto a = std::filesystem::path{"/logs/a.zevtc"};
    const auto b = std::filesystem::path{"/logs/b.zevtc"};
    CandidateSnapshotTracker tracker;

    auto initial = tracker.reconcile(DirectorySnapshot{
        .root_available = true,
        .complete = true,
        .observations =
            {
                ports::LogFileObservation{.canonical_path = b, .size = 2, .last_write_time = {}},
                ports::LogFileObservation{.canonical_path = a, .size = 1, .last_write_time = {}},
            },
        .seen_candidate_paths = {},
        .issues = {},
    });
    MANNY_CHECK(suite, initial.observations.front().canonical_path == a);
    MANNY_CHECK(suite, initial.removed_paths.empty());
    MANNY_CHECK(suite, tracker.retained_count() == 2);

    auto incomplete = tracker.reconcile(DirectorySnapshot{
        .root_available = true,
        .complete = false,
        .observations =
            {
                ports::LogFileObservation{.canonical_path = a, .size = 1, .last_write_time = {}},
            },
        .seen_candidate_paths = {a},
        .issues =
            {
                ports::LogCandidateIssue{.path = b, .message = "transient access failure"},
            },
    });
    MANNY_CHECK(suite, !incomplete.scan_complete);
    MANNY_CHECK(suite, incomplete.issues.size() == 1);
    MANNY_CHECK(suite, incomplete.removed_paths.empty());
    MANNY_CHECK(suite, tracker.retained_count() == 2);

    auto authoritative = tracker.reconcile(DirectorySnapshot{
        .root_available = true,
        .complete = true,
        .observations =
            {
                ports::LogFileObservation{.canonical_path = a, .size = 1, .last_write_time = {}},
            },
        .seen_candidate_paths = {},
        .issues = {},
    });
    MANNY_CHECK(suite, authoritative.removed_paths.size() == 1);
    MANNY_CHECK(suite, authoritative.removed_paths.front() == b);
    MANNY_CHECK(suite, tracker.retained_count() == 1);

    auto unavailable = tracker.reconcile(DirectorySnapshot{
        .root_available = false,
        .complete = true,
        .observations = {},
        .seen_candidate_paths = {},
        .issues = {},
    });
    MANNY_CHECK(suite, unavailable.removed_paths.size() == 1);
    MANNY_CHECK(suite, unavailable.removed_paths.front() == a);
    MANNY_CHECK(suite, tracker.retained_count() == 0);

    tracker.clear();
    MANNY_CHECK(suite, tracker.retained_count() == 0);
}

} // namespace

void run_polling_log_candidate_source_tests(TestSuite& suite) {
    configuration_tests(suite);
    missing_creation_and_recursive_tests(suite);
    non_recursive_and_root_validation_tests(suite);
    live_reconfiguration_tests(suite);
    resource_limit_and_cancellation_tests(suite);
    cutoff_and_large_archive_tests(suite);
    incomplete_snapshot_reconciliation_tests(suite);
}

} // namespace manny_uploader::test
