#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace active_runtime_fixture {

using Bytes = std::vector<std::uint8_t>;

inline void put16(Bytes& bytes, std::size_t offset, std::uint16_t value) {
    for (std::size_t index = 0; index < 2; ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8));
    }
}
inline void put32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8));
    }
}
inline void append(Bytes& bytes, std::string_view text) {
    for (char value : text) {
        bytes.push_back(static_cast<std::uint8_t>(value));
    }
}
inline std::uint32_t crc32(const Bytes& bytes) {
    std::uint32_t crc = 0xffffffffU;
    for (auto value : bytes) {
        crc ^= value;
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

inline Bytes archive() {
    Bytes payload(16 + 4 + 96 + 4 + 64);
    constexpr std::string_view magic = "EVTC20261016";
    for (std::size_t index = 0; index < magic.size(); ++index) {
        payload[index] = static_cast<std::uint8_t>(magic[index]);
    }
    payload[12] = 1;
    payload[13] = 123;
    put32(payload, 16, 1);
    payload[20] = 42;
    constexpr std::string_view account{"Fixture\0:Synthetic.1234\0", 24};
    for (std::size_t index = 0; index < account.size(); ++index) {
        payload[20 + 28 + index] = static_cast<std::uint8_t>(account[index]);
    }
    payload[120 + 8] = 42;
    payload[120 + 56] = 13;
    constexpr std::string_view name = "fixture.evtc";
    const auto crc = crc32(payload);
    const auto length = static_cast<std::uint32_t>(payload.size());
    Bytes result(30);
    put32(result, 0, 0x04034b50);
    put16(result, 4, 20);
    put32(result, 14, crc);
    put32(result, 18, length);
    put32(result, 22, length);
    put16(result, 26, static_cast<std::uint16_t>(name.size()));
    append(result, name);
    result.insert(result.end(), payload.begin(), payload.end());
    const auto directory_offset = static_cast<std::uint32_t>(result.size());
    Bytes directory(46);
    put32(directory, 0, 0x02014b50);
    put16(directory, 4, 20);
    put16(directory, 6, 20);
    put32(directory, 16, crc);
    put32(directory, 20, length);
    put32(directory, 24, length);
    put16(directory, 28, static_cast<std::uint16_t>(name.size()));
    append(directory, name);
    Bytes end(22);
    put32(end, 0, 0x06054b50);
    put16(end, 8, 1);
    put16(end, 10, 1);
    put32(end, 12, static_cast<std::uint32_t>(directory.size()));
    put32(end, 16, directory_offset);
    result.insert(result.end(), directory.begin(), directory.end());
    result.insert(result.end(), end.begin(), end.end());
    return result;
}

inline bool stage(const std::filesystem::path& game, std::size_t cycle) {
    const auto directory = game / "arcdps.cbtlogs";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        return false;
    }
    const auto bytes = archive();
    // Two active workers plus one queued request exercise both cancellation paths.
    for (unsigned index = 0; index < 3; ++index) {
        std::ofstream file{directory / ("fixture-" + std::to_string(cycle) + "-" +
                                        std::to_string(index) + ".zevtc"),
                           std::ios::binary};
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        file.close();
        if (file.fail()) {
            return false;
        }
    }
    return true;
}

} // namespace active_runtime_fixture
