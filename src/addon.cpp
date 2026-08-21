#include "manny_uploader/project_info.hpp"

#if defined(_WIN32)
#define MANNY_EXPORT extern "C" __declspec(dllexport)
#elif defined(__GNUC__)
#define MANNY_EXPORT extern "C" __attribute__((visibility("default")))
#else
#define MANNY_EXPORT extern "C"
#endif

// Temporary bootstrap export. Phase 0 will replace this with Nexus GetAddonDef integration.
MANNY_EXPORT const char* MannyUploaderBootstrapName() noexcept {
    return manny_uploader::project_info().name.data();
}
