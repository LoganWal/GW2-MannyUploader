#include "manny_uploader/project_info.hpp"
#include "support/test_suite.hpp"

#include <string_view>

namespace manny_uploader::test {

void run_project_info_tests(TestSuite& suite) {
    const auto& info = manny_uploader::project_info();

    MANNY_CHECK(suite, info.name == std::string_view{"GW2 Manny Uploader"});
    MANNY_CHECK(suite, info.version_text == std::string_view{"0.1.0-dev"});
    MANNY_CHECK(suite, info.version.major == 0);
    MANNY_CHECK(suite, info.version.minor == 1);
    MANNY_CHECK(suite, info.version.patch == 0);
}

} // namespace manny_uploader::test
