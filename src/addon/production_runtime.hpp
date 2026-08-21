#pragma once

#include "manny_uploader/addon/addon_lifecycle.hpp"

#include <expected>
#include <memory>

namespace manny_uploader::addon {

[[nodiscard]] std::expected<std::unique_ptr<IAddonRuntime>, AddonRuntimeError>
create_production_runtime(const AddonPaths& paths);

} // namespace manny_uploader::addon
