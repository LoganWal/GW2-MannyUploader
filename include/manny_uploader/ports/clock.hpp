#pragma once

#include <chrono>

namespace manny_uploader::ports {

class IClock {
  public:
    virtual ~IClock() = default;

    [[nodiscard]] virtual std::chrono::system_clock::time_point system_now() const noexcept = 0;
    [[nodiscard]] virtual std::chrono::steady_clock::time_point steady_now() const noexcept = 0;
};

} // namespace manny_uploader::ports
