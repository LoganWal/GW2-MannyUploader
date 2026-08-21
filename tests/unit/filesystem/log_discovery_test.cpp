#include "manny_uploader/filesystem/log_discovery.hpp"
#include "support/test_suite.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>

namespace manny_uploader::test {
namespace {

using filesystem::FileObservation;
using filesystem::FileStabilityTracker;
using filesystem::LogDeduplicator;
using filesystem::LogDiscoveryErrorCode;
using filesystem::LogDiscoveryPipeline;

[[nodiscard]] std::filesystem::file_time_type write_time(std::int64_t seconds) {
    return std::filesystem::file_time_type{} + std::chrono::seconds{seconds};
}

[[nodiscard]] FileObservation observation(std::string path, std::uintmax_t size = 4096,
                                          std::int64_t written_at = 1) {
    return FileObservation{
        .canonical_path = std::move(path),
        .size = size,
        .last_write_time = write_time(written_at),
    };
}

[[nodiscard]] domain::LogFileIdentity identity(std::string path, std::uintmax_t size = 4096,
                                               std::int64_t written_at = 1) {
    return domain::LogFileIdentity{
        .canonical_path = std::move(path),
        .size = size,
        .last_write_time = write_time(written_at),
    };
}

void candidate_tests(TestSuite& suite) {
    MANNY_CHECK(suite, filesystem::is_zevtc_candidate("logs/example.zevtc"));
    MANNY_CHECK(suite, filesystem::is_zevtc_candidate("logs/example.ZEVTC"));
    MANNY_CHECK(suite, filesystem::is_zevtc_candidate("logs/example.ZeVtC"));
    MANNY_CHECK(suite, !filesystem::is_zevtc_candidate("logs/example.evtc"));
    MANNY_CHECK(suite, !filesystem::is_zevtc_candidate("logs/example.zevtc.tmp"));
    MANNY_CHECK(suite, !filesystem::is_zevtc_candidate("logs/zevtc"));
    MANNY_CHECK(suite, !filesystem::is_zevtc_candidate({}));
}

void stability_validation_tests(TestSuite& suite) {
    const auto invalid_zero = FileStabilityTracker::create(0);
    MANNY_CHECK(suite, !invalid_zero.has_value());
    MANNY_CHECK(suite, invalid_zero.error().code == LogDiscoveryErrorCode::InvalidRequiredMatches);

    const auto invalid_one = FileStabilityTracker::create(1);
    MANNY_CHECK(suite, !invalid_one.has_value());
    MANNY_CHECK(suite, invalid_one.error().code == LogDiscoveryErrorCode::InvalidRequiredMatches);

    auto created = FileStabilityTracker::create();
    MANNY_CHECK(suite, created.has_value());
    auto tracker = std::move(created.value());

    const auto empty = tracker.observe(observation(""));
    MANNY_CHECK(suite, !empty.has_value());
    MANNY_CHECK(suite, empty.error().code == LogDiscoveryErrorCode::EmptyPath);

    const auto unsupported = tracker.observe(observation("logs/example.evtc"));
    MANNY_CHECK(suite, !unsupported.has_value());
    MANNY_CHECK(suite, unsupported.error().code == LogDiscoveryErrorCode::UnsupportedExtension);
    MANNY_CHECK(suite, tracker.tracked_count() == 0);
}

void repeated_observation_tests(TestSuite& suite) {
    auto created = FileStabilityTracker::create(3);
    MANNY_CHECK(suite, created.has_value());
    auto tracker = std::move(created.value());

    const auto first = tracker.observe(observation("logs/example.zevtc"));
    MANNY_CHECK(suite, first.has_value());
    MANNY_CHECK(suite, !first->has_value());
    MANNY_CHECK(suite, tracker.tracked_count() == 1);

    const auto second = tracker.observe(observation("logs/example.zevtc"));
    MANNY_CHECK(suite, second.has_value());
    MANNY_CHECK(suite, !second->has_value());

    const auto third = tracker.observe(observation("logs/example.zevtc"));
    MANNY_CHECK(suite, third.has_value());
    MANNY_CHECK(suite, third->has_value());
    MANNY_CHECK(suite, third->value().canonical_path == "logs/example.zevtc");
    MANNY_CHECK(suite, third->value().size == 4096);
    MANNY_CHECK(suite, third->value().last_write_time == write_time(1));
    MANNY_CHECK(suite, tracker.tracked_count() == 0);

    const auto after_emission = tracker.observe(observation("logs/example.zevtc"));
    MANNY_CHECK(suite, after_emission.has_value());
    MANNY_CHECK(suite, !after_emission->has_value());
}

void reset_and_parallel_tracking_tests(TestSuite& suite) {
    auto created = FileStabilityTracker::create();
    MANNY_CHECK(suite, created.has_value());
    auto tracker = std::move(created.value());

    MANNY_CHECK(suite, tracker.observe(observation("logs/a.zevtc", 100, 1)).has_value());
    MANNY_CHECK(suite, tracker.observe(observation("logs/b.zevtc", 200, 2)).has_value());
    MANNY_CHECK(suite, tracker.tracked_count() == 2);

    const auto size_changed = tracker.observe(observation("logs/a.zevtc", 150, 1));
    MANNY_CHECK(suite, size_changed.has_value());
    MANNY_CHECK(suite, !size_changed->has_value());

    const auto b_stable = tracker.observe(observation("logs/b.zevtc", 200, 2));
    MANNY_CHECK(suite, b_stable.has_value());
    MANNY_CHECK(suite, b_stable->has_value());

    const auto time_changed = tracker.observe(observation("logs/a.zevtc", 150, 3));
    MANNY_CHECK(suite, time_changed.has_value());
    MANNY_CHECK(suite, !time_changed->has_value());

    const auto a_stable = tracker.observe(observation("logs/a.zevtc", 150, 3));
    MANNY_CHECK(suite, a_stable.has_value());
    MANNY_CHECK(suite, a_stable->has_value());
    MANNY_CHECK(suite, a_stable->value().size == 150);
    MANNY_CHECK(suite, a_stable->value().last_write_time == write_time(3));
    MANNY_CHECK(suite, tracker.tracked_count() == 0);

    MANNY_CHECK(suite, tracker.observe(observation("logs/forgotten.zevtc")).has_value());
    tracker.forget("logs/forgotten.zevtc");
    MANNY_CHECK(suite, tracker.tracked_count() == 0);
    MANNY_CHECK(suite, tracker.observe(observation("logs/one.zevtc")).has_value());
    MANNY_CHECK(suite, tracker.observe(observation("logs/two.zevtc")).has_value());
    tracker.clear();
    MANNY_CHECK(suite, tracker.tracked_count() == 0);
}

void dedupe_tests(TestSuite& suite) {
    const auto invalid = LogDeduplicator::create(0);
    MANNY_CHECK(suite, !invalid.has_value());
    MANNY_CHECK(suite, invalid.error().code == LogDiscoveryErrorCode::InvalidDedupeCapacity);

    const auto empty_key = filesystem::make_log_dedupe_key(identity(""));
    MANNY_CHECK(suite, !empty_key.has_value());
    MANNY_CHECK(suite, empty_key.error().code == LogDiscoveryErrorCode::EmptyPath);

    const auto unsupported_key = filesystem::make_log_dedupe_key(identity("logs/a.evtc"));
    MANNY_CHECK(suite, !unsupported_key.has_value());
    MANNY_CHECK(suite, unsupported_key.error().code == LogDiscoveryErrorCode::UnsupportedExtension);

    auto created = LogDeduplicator::create(2);
    MANNY_CHECK(suite, created.has_value());
    auto dedupe = std::move(created.value());
    MANNY_CHECK(suite, dedupe.capacity() == 2);

    const auto a = filesystem::make_log_dedupe_key(identity("logs/a.zevtc", 100, 1));
    const auto a_new_size = filesystem::make_log_dedupe_key(identity("logs/a.zevtc", 101, 1));
    const auto a_new_time = filesystem::make_log_dedupe_key(identity("logs/a.zevtc", 100, 4));
    const auto b = filesystem::make_log_dedupe_key(identity("logs/b.zevtc", 200, 2));
    const auto c = filesystem::make_log_dedupe_key(identity("logs/c.zevtc", 300, 3));
    MANNY_CHECK(suite, a.has_value());
    MANNY_CHECK(suite, a_new_size.has_value());
    MANNY_CHECK(suite, a_new_time.has_value());
    MANNY_CHECK(suite, b.has_value());
    MANNY_CHECK(suite, c.has_value());

    MANNY_CHECK(suite, dedupe.remember(a.value()));
    MANNY_CHECK(suite, !dedupe.remember(a.value()));
    MANNY_CHECK(suite, dedupe.forget(a.value()));
    MANNY_CHECK(suite, !dedupe.forget(a.value()));
    MANNY_CHECK(suite, dedupe.remember(a.value()));
    MANNY_CHECK(suite, dedupe.remember(a_new_size.value()));
    MANNY_CHECK(suite, dedupe.size() == 2);

    MANNY_CHECK(suite, dedupe.remember(b.value()));
    MANNY_CHECK(suite, !dedupe.contains(a.value()));
    MANNY_CHECK(suite, dedupe.contains(a_new_size.value()));
    MANNY_CHECK(suite, dedupe.contains(b.value()));

    MANNY_CHECK(suite, dedupe.remember(c.value()));
    MANNY_CHECK(suite, !dedupe.contains(a_new_size.value()));
    MANNY_CHECK(suite, dedupe.contains(b.value()));
    MANNY_CHECK(suite, dedupe.contains(c.value()));

    auto identity_created = LogDeduplicator::create(3);
    MANNY_CHECK(suite, identity_created.has_value());
    auto identity_dedupe = std::move(identity_created.value());
    MANNY_CHECK(suite, identity_dedupe.remember(a.value()));
    MANNY_CHECK(suite, identity_dedupe.remember(a_new_size.value()));
    MANNY_CHECK(suite, identity_dedupe.remember(a_new_time.value()));

    dedupe.clear();
    MANNY_CHECK(suite, dedupe.size() == 0);
    MANNY_CHECK(suite, dedupe.remember(a.value()));
}

void discovery_pipeline_tests(TestSuite& suite) {
    const auto invalid_stability = LogDiscoveryPipeline::create(1, 2);
    MANNY_CHECK(suite, !invalid_stability.has_value());
    MANNY_CHECK(suite,
                invalid_stability.error().code == LogDiscoveryErrorCode::InvalidRequiredMatches);

    const auto invalid_dedupe = LogDiscoveryPipeline::create(2, 0);
    MANNY_CHECK(suite, !invalid_dedupe.has_value());
    MANNY_CHECK(suite, invalid_dedupe.error().code == LogDiscoveryErrorCode::InvalidDedupeCapacity);

    auto created = LogDiscoveryPipeline::create(2, 2);
    MANNY_CHECK(suite, created.has_value());
    auto pipeline = std::move(created.value());

    const auto first = pipeline.observe(observation("logs/pipeline.zevtc", 100, 1));
    MANNY_CHECK(suite, first.has_value());
    MANNY_CHECK(suite, !first->has_value());
    const auto stable = pipeline.observe(observation("logs/pipeline.zevtc", 100, 1));
    MANNY_CHECK(suite, stable.has_value());
    MANNY_CHECK(suite, stable->has_value());
    MANNY_CHECK(suite, pipeline.dedupe_size() == 1);

    MANNY_CHECK(suite, pipeline.observe(observation("logs/pipeline.zevtc", 100, 1)).has_value());
    const auto duplicate = pipeline.observe(observation("logs/pipeline.zevtc", 100, 1));
    MANNY_CHECK(suite, duplicate.has_value());
    MANNY_CHECK(suite, !duplicate->has_value());
    MANNY_CHECK(suite, pipeline.dedupe_size() == 1);

    MANNY_CHECK(suite, pipeline.observe(observation("logs/pipeline.zevtc", 101, 1)).has_value());
    const auto changed = pipeline.observe(observation("logs/pipeline.zevtc", 101, 1));
    MANNY_CHECK(suite, changed.has_value());
    MANNY_CHECK(suite, changed->has_value());
    MANNY_CHECK(suite, changed->value().size == 101);
    MANNY_CHECK(suite, pipeline.dedupe_size() == 2);

    MANNY_CHECK(suite, pipeline.release(changed->value()).has_value());
    MANNY_CHECK(suite, pipeline.dedupe_size() == 1);

    MANNY_CHECK(suite, pipeline.observe(observation("logs/forgotten.zevtc", 1, 1)).has_value());
    pipeline.forget("logs/forgotten.zevtc");
    MANNY_CHECK(suite, pipeline.tracked_count() == 0);
    pipeline.clear();
    MANNY_CHECK(suite, pipeline.dedupe_size() == 0);
}

} // namespace

void run_log_discovery_tests(TestSuite& suite) {
    candidate_tests(suite);
    stability_validation_tests(suite);
    repeated_observation_tests(suite);
    reset_and_parallel_tracking_tests(suite);
    dedupe_tests(suite);
    discovery_pipeline_tests(suite);
}

} // namespace manny_uploader::test
