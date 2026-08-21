#include "manny_uploader/support/secret_value.hpp"

#include <algorithm>
#include <utility>

namespace manny_uploader::support {

void secure_erase(std::span<std::byte> bytes) noexcept {
    volatile auto* cursor = reinterpret_cast<volatile std::byte*>(bytes.data());
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        cursor[index] = std::byte{};
    }
}

SecretValue::SecretValue(std::vector<std::byte> bytes) noexcept : bytes_{std::move(bytes)} {}

SecretValue::~SecretValue() {
    clear();
}

SecretValue::SecretValue(SecretValue&& other) noexcept : bytes_{std::move(other.bytes_)} {}

SecretValue& SecretValue::operator=(SecretValue&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    clear();
    bytes_ = std::move(other.bytes_);
    return *this;
}

SecretValue SecretValue::from_text(std::string_view text) {
    const auto characters = std::span{text.data(), text.size()};
    const auto bytes = std::as_bytes(characters);
    return SecretValue{std::vector<std::byte>{bytes.begin(), bytes.end()}};
}

std::span<const std::byte> SecretValue::bytes() const noexcept {
    return bytes_;
}

std::size_t SecretValue::size() const noexcept {
    return bytes_.size();
}

bool SecretValue::empty() const noexcept {
    return bytes_.empty();
}

void SecretValue::clear() noexcept {
    secure_erase(std::span{bytes_});
    bytes_.clear();
}

bool operator==(const SecretValue& left, const SecretValue& right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    std::byte difference{};
    for (std::size_t index = 0; index < left.size(); ++index) {
        difference |= left.bytes_[index] ^ right.bytes_[index];
    }
    return difference == std::byte{};
}

} // namespace manny_uploader::support
