#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

namespace manny_uploader::support {

enum class ReportPermalinkOrigin {
    DpsReport,
    DpsReportAlternate,
    WvwReport,
};

[[nodiscard]] inline std::optional<ReportPermalinkOrigin>
report_permalink_origin(std::string_view value) noexcept {
    constexpr std::size_t maximum_length = 2048;
    constexpr std::string_view scheme = "https://";
    if (!value.starts_with(scheme) || value.size() > maximum_length) {
        return std::nullopt;
    }

    const auto path_start = value.find('/', scheme.size());
    if (path_start == std::string_view::npos || path_start + 1 >= value.size()) {
        return std::nullopt;
    }
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x21U || byte > 0x7eU || character == '@' || character == '?' ||
            character == '#' || character == '\\' || character == '"') {
            return std::nullopt;
        }
    }

    const auto authority = value.substr(scheme.size(), path_start - scheme.size());
    if (authority == "dps.report") {
        return ReportPermalinkOrigin::DpsReport;
    }
    if (authority == "b.dps.report") {
        return ReportPermalinkOrigin::DpsReportAlternate;
    }
    if (authority == "wvw.report") {
        return ReportPermalinkOrigin::WvwReport;
    }
    return std::nullopt;
}

} // namespace manny_uploader::support
