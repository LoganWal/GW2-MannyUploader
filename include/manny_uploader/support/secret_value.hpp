#pragma once

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace manny_uploader::support {

void secure_erase(std::span<std::byte> bytes) noexcept;

class SecretValue {
  public:
    SecretValue() = default;
    explicit SecretValue(std::vector<std::byte> bytes) noexcept;
    ~SecretValue();

    SecretValue(const SecretValue&) = delete;
    SecretValue& operator=(const SecretValue&) = delete;
    SecretValue(SecretValue&& other) noexcept;
    SecretValue& operator=(SecretValue&& other) noexcept;

    [[nodiscard]] static SecretValue from_text(std::string_view text);

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    void clear() noexcept;

    friend bool operator==(const SecretValue& left, const SecretValue& right) noexcept;

  private:
    std::vector<std::byte> bytes_;
};

} // namespace manny_uploader::support
