#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

namespace manny_uploader::ports {

struct LogFileObservation {
    std::filesystem::path canonical_path;
    std::uintmax_t size{};
    std::filesystem::file_time_type last_write_time{};
};

struct LogCandidateIssue {
    std::filesystem::path path;
    std::string message;
};

struct LogCandidateBatch {
    bool root_available{};
    bool scan_complete{};
    std::vector<LogFileObservation> observations;
    std::vector<std::filesystem::path> removed_paths;
    std::vector<LogCandidateIssue> issues;
};

enum class LogCandidateSourceErrorCode : std::uint8_t {
    InvalidConfiguration,
    Cancelled,
    ScanFailed,
    ResourceLimit,
};

struct LogCandidateSourceError {
    LogCandidateSourceErrorCode code;
    std::string message;
};

class ILogCandidateSource {
  public:
    virtual ~ILogCandidateSource() = default;

    [[nodiscard]] virtual std::expected<LogCandidateBatch, LogCandidateSourceError>
    poll(const std::stop_token& stop_token) = 0;
};

} // namespace manny_uploader::ports
