#include "manny_uploader/addon/addon_lifecycle.hpp"
#include "manny_uploader/project_info.hpp"

#include "production_runtime.hpp"

#include <Nexus.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace manny_uploader::addon {
namespace {

static_assert(NEXUS_API_VERSION == 6, "Review the Nexus adapter before changing API versions");
static_assert(IMGUI_VERSION_NUM == 18000, "Nexus and addon ImGui ABIs must match");

// Development-only identity until Raidcore assigns or confirms the public listing signature.
inline constexpr std::uint32_t provisional_addon_signature = 0xB2D15965U;
inline constexpr char addon_name[] = "GW2 Manny Uploader";
inline constexpr char addon_author[] = "LoganWal";
inline constexpr char addon_description[] =
    "Uploads arcdps logs to dps.report, GW2Wingman, DonBot, and broadcaster Twitch chat.";
inline constexpr char addon_update_link[] = "https://github.com/LoganWal/GW2-MannyUploader";
inline constexpr char addon_storage_name[] = "GW2MannyUploader";
inline constexpr char log_channel[] = "GW2 Manny Uploader";

class NexusHost final : public IAddonHost {
  public:
    [[nodiscard]] std::expected<void, AddonHostError> attach(AddonAPI_t* api) {
        if (api == nullptr || api->ImguiContext == nullptr || api->ImguiMalloc == nullptr ||
            api->ImguiFree == nullptr || api->GUI_Register == nullptr ||
            api->GUI_Deregister == nullptr || api->Log == nullptr ||
            api->Paths_GetGameDirectory == nullptr || api->Paths_GetAddonDirectory == nullptr) {
            return std::unexpected(AddonHostError{
                .code = AddonHostErrorCode::InvalidApi,
                .message = "Nexus supplied an incomplete addon API",
            });
        }

        api_ = api;
        using ImGuiAllocate = void* (*)(std::size_t, void*);
        using ImGuiFree = void (*)(void*, void*);
        ImGui::SetCurrentContext(static_cast<ImGuiContext*>(api_->ImguiContext));
        ImGui::SetAllocatorFunctions(reinterpret_cast<ImGuiAllocate>(api_->ImguiMalloc),
                                     reinterpret_cast<ImGuiFree>(api_->ImguiFree));
        return {};
    }

    void detach() noexcept {
        api_ = nullptr;
    }

    [[nodiscard]] std::expected<AddonPaths, AddonHostError> paths() const override {
        if (api_ == nullptr) {
            return std::unexpected(AddonHostError{
                .code = AddonHostErrorCode::InvalidApi,
                .message = "Nexus API is not attached",
            });
        }

        const char* game_directory = api_->Paths_GetGameDirectory();
        const char* addon_directory = api_->Paths_GetAddonDirectory(addon_storage_name);
        if (game_directory == nullptr || game_directory[0] == '\0' || addon_directory == nullptr ||
            addon_directory[0] == '\0') {
            return std::unexpected(AddonHostError{
                .code = AddonHostErrorCode::InvalidPath,
                .message = "Nexus did not provide valid game and addon directories",
            });
        }

        return AddonPaths{
            .game_directory = std::filesystem::path{game_directory},
            .addon_directory = std::filesystem::path{addon_directory},
        };
    }

    [[nodiscard]] std::expected<void, AddonHostError>
    register_render(RenderCallbackKind kind, RenderCallback callback) override {
        if (api_ == nullptr || callback == nullptr) {
            return std::unexpected(AddonHostError{
                .code = AddonHostErrorCode::RegistrationFailed,
                .message = "Cannot register a Nexus render callback",
            });
        }
        api_->GUI_Register(kind == RenderCallbackKind::Main ? RT_Render : RT_OptionsRender,
                           callback);
        return {};
    }

    void deregister_render(RenderCallback callback) noexcept override {
        if (api_ != nullptr && callback != nullptr) {
            api_->GUI_Deregister(callback);
        }
    }

    void log(AddonLogLevel level, std::string_view message) noexcept override {
        if (api_ == nullptr) {
            return;
        }
        const auto native_level = [level] {
            switch (level) {
            case AddonLogLevel::Critical:
                return LOGL_CRITICAL;
            case AddonLogLevel::Warning:
                return LOGL_WARNING;
            case AddonLogLevel::Info:
                return LOGL_INFO;
            case AddonLogLevel::Debug:
                return LOGL_DEBUG;
            }
            return LOGL_INFO;
        }();
        std::array<char, 512> buffer{};
        const auto length = std::min(message.size(), buffer.size() - 1);
        if (length > 0) {
            std::memcpy(buffer.data(), message.data(), length);
        }
        buffer[length] = '\0';
        api_->Log(native_level, log_channel, buffer.data());
    }

  private:
    AddonAPI_t* api_{};
};

class NexusRuntimeFactory final : public IAddonRuntimeFactory {
  public:
    [[nodiscard]] std::expected<std::unique_ptr<IAddonRuntime>, AddonRuntimeError>
    create(const AddonPaths& paths) override {
        return create_production_runtime(paths);
    }
};

NexusHost nexus_host;
AddonLifecycle addon_lifecycle;
NexusRuntimeFactory runtime_factory;

void render_main_callback() {
    addon_lifecycle.render_main();
}

void render_options_callback() {
    addon_lifecycle.render_options();
}

void load(AddonAPI_t* api) noexcept {
    try {
        auto attached = nexus_host.attach(api);
        if (!attached) {
            return;
        }

        auto loaded = addon_lifecycle.load(
            nexus_host, runtime_factory,
            AddonCallbacks{.main = render_main_callback, .options = render_options_callback});
        if (!loaded) {
            nexus_host.log(AddonLogLevel::Critical, loaded.error().message);
            nexus_host.detach();
        }
    } catch (...) {
        nexus_host.log(AddonLogLevel::Critical,
                       "An exception was contained at the Nexus load callback boundary");
        addon_lifecycle.unload();
        nexus_host.detach();
    }
}

void unload() noexcept {
    addon_lifecycle.unload();
    nexus_host.detach();
}

AddonDefinition_t addon_definition{
    .Signature = provisional_addon_signature,
    .APIVersion = NEXUS_API_VERSION,
    .Name = addon_name,
    .Version =
        AddonVersion_t{
            .Major = project_info().version.major,
            .Minor = project_info().version.minor,
            .Build = project_info().version.patch,
            .Revision = 0,
        },
    .Author = addon_author,
    .Description = addon_description,
    .Load = load,
    .Unload = unload,
    .Flags = AF_None,
    .Provider = UP_GitHub,
    .UpdateLink = addon_update_link,
};

} // namespace
} // namespace manny_uploader::addon

extern "C" __declspec(dllexport) AddonDefinition_t* GetAddonDef() noexcept {
    return &manny_uploader::addon::addon_definition;
}
