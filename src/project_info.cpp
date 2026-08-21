#include "manny_uploader/project_info.hpp"

namespace manny_uploader {

const ProjectInfo& project_info() noexcept {
    static constexpr ProjectInfo info{
        .name = "GW2 Manny Uploader",
        .version_text = "0.1.0-dev",
        .version = {.major = 0, .minor = 1, .patch = 0},
    };

    return info;
}

} // namespace manny_uploader
