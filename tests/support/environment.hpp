#pragma once

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace manny_uploader::test::environment {

[[nodiscard]] inline std::optional<std::string> value(std::string_view name) {
    const std::string owned_name{name};
#if defined(_MSC_VER)
    char* raw_value{};
    std::size_t value_size{};
    if (_dupenv_s(&raw_value, &value_size, owned_name.c_str()) != 0 || raw_value == nullptr) {
        std::free(raw_value);
        return std::nullopt;
    }
    const std::unique_ptr<char, decltype(&std::free)> value{raw_value, &std::free};
    return std::string{value.get()};
#else
    const auto* value = std::getenv(owned_name.c_str());
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string{value};
#endif
}

} // namespace manny_uploader::test::environment
