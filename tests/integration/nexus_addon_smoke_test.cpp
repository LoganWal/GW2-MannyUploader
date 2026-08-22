#include <Nexus.h>
#include <imgui.h>
#include <windows.h>

#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using GetAddonDefinition = AddonDefinition_t* (*)();
constexpr std::size_t hot_load_cycles = 10;

std::array<GUI_RENDER, 2> registered_callbacks{};
std::array<GUI_RENDER, 2> deregistered_callbacks{};
std::size_t registration_count{};
std::size_t deregistration_count{};
INPUTBINDS_PROCESS registered_input_bind{};
std::size_t input_registration_count{};
std::size_t input_deregistration_count{};
std::size_t quick_access_registration_count{};
std::size_t quick_access_deregistration_count{};
Texture_t shortcut_texture{.Width = 32, .Height = 32, .Resource = &shortcut_texture};
std::vector<std::string> lifecycle_events;
bool host_contract_valid{true};
std::string game_directory;
std::string addon_directory;

void* host_allocate(std::size_t size, void*) {
    return std::malloc(size);
}

void host_free(void* memory, void*) {
    std::free(memory);
}

void register_render(ERenderType type, GUI_RENDER callback) {
    if (registration_count < registered_callbacks.size()) {
        registered_callbacks[registration_count++] = callback;
    }
    lifecycle_events.emplace_back(type == RT_Render ? "register_main" : "register_options");
}

void deregister_render(GUI_RENDER callback) {
    if (deregistration_count < deregistered_callbacks.size()) {
        deregistered_callbacks[deregistration_count++] = callback;
    }
    lifecycle_events.emplace_back(callback == registered_callbacks[1] ? "deregister_options"
                                                                      : "deregister_main");
}

void register_input_bind(const char* identifier, INPUTBINDS_PROCESS callback,
                         const char* default_bind) {
    host_contract_valid = host_contract_valid && identifier != nullptr && default_bind != nullptr &&
                          std::string{identifier} == "KB_GW2_MANNY_UPLOADER_TOGGLE" &&
                          std::string{default_bind} == "ALT+SHIFT+M" && callback != nullptr;
    registered_input_bind = callback;
    ++input_registration_count;
    lifecycle_events.emplace_back("register_input");
}

void deregister_input_bind(const char* identifier) {
    host_contract_valid = host_contract_valid && identifier != nullptr &&
                          std::string{identifier} == "KB_GW2_MANNY_UPLOADER_TOGGLE";
    ++input_deregistration_count;
    lifecycle_events.emplace_back("deregister_input");
}

Texture_t* create_texture(const char* identifier, void* bytes, std::uint64_t size) {
    const auto* png = static_cast<const unsigned char*>(bytes);
    host_contract_valid = host_contract_valid && identifier != nullptr &&
                          std::string{identifier} == "TEX_GW2_MANNY_UPLOADER" && png != nullptr &&
                          size == 222 && png[0] == 0x89 && png[1] == 0x50 && png[2] == 0x4e &&
                          png[3] == 0x47;
    lifecycle_events.emplace_back("create_texture");
    return &shortcut_texture;
}

void add_quick_access(const char* identifier, const char* texture_identifier,
                      const char* hover_texture_identifier, const char* keybind_identifier,
                      const char* tooltip) {
    host_contract_valid = host_contract_valid && identifier != nullptr &&
                          texture_identifier != nullptr && hover_texture_identifier != nullptr &&
                          keybind_identifier != nullptr && tooltip != nullptr &&
                          std::string{identifier} == "QA_GW2_MANNY_UPLOADER" &&
                          std::string{texture_identifier} == "TEX_GW2_MANNY_UPLOADER" &&
                          std::string{hover_texture_identifier} == "TEX_GW2_MANNY_UPLOADER" &&
                          std::string{keybind_identifier} == "KB_GW2_MANNY_UPLOADER_TOGGLE" &&
                          std::string{tooltip} == "Toggle GW2 Manny Uploader";
    ++quick_access_registration_count;
    lifecycle_events.emplace_back("register_quick_access");
}

void remove_quick_access(const char* identifier) {
    host_contract_valid = host_contract_valid && identifier != nullptr &&
                          std::string{identifier} == "QA_GW2_MANNY_UPLOADER";
    ++quick_access_deregistration_count;
    lifecycle_events.emplace_back("deregister_quick_access");
}

void log_message(ELogLevel, const char*, const char*) {}

const char* get_game_directory() {
    return game_directory.c_str();
}

const char* get_addon_directory(const char*) {
    return addon_directory.c_str();
}

[[nodiscard]] bool check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

} // namespace

int main(int argument_count, char** arguments) {
    if (argument_count != 2) {
        std::cerr << "Expected the addon DLL path\n";
        return 1;
    }

    const auto root = std::filesystem::temp_directory_path() /
                      ("gw2-manny-nexus-smoke-" + std::to_string(GetCurrentProcessId()));
    const auto game = root / "Guild Wars 2";
    const auto addon = game / "addons" / "GW2MannyUploader";
    std::error_code filesystem_error;
    std::filesystem::create_directories(game, filesystem_error);
    if (!check(!filesystem_error, "Unable to create smoke-test directories")) {
        return 1;
    }
    game_directory = game.string();
    addon_directory = addon.string();

    ImGui::SetAllocatorFunctions(host_allocate, host_free);
    ImGuiContext* context = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.Fonts->AddFontDefault();
    unsigned char* font_pixels{};
    int font_width{};
    int font_height{};
    io.Fonts->GetTexDataAsRGBA32(&font_pixels, &font_width, &font_height);

    bool passed = true;
    for (std::size_t cycle = 0; cycle < hot_load_cycles; ++cycle) {
        registered_callbacks.fill(nullptr);
        deregistered_callbacks.fill(nullptr);
        registration_count = 0;
        deregistration_count = 0;
        registered_input_bind = nullptr;
        input_registration_count = 0;
        input_deregistration_count = 0;
        quick_access_registration_count = 0;
        quick_access_deregistration_count = 0;
        lifecycle_events.clear();
        host_contract_valid = true;

        HMODULE module = LoadLibraryA(arguments[1]);
        if (!check(module != nullptr, "Unable to load the addon DLL")) {
            passed = false;
            break;
        }
        const auto get_definition =
            std::bit_cast<GetAddonDefinition>(GetProcAddress(module, "GetAddonDef"));
        if (!check(get_definition != nullptr, "GetAddonDef is not exported")) {
            passed = false;
            (void)FreeLibrary(module);
            break;
        }

        AddonDefinition_t* definition = get_definition();
        passed =
            check(definition != nullptr, "GetAddonDef returned null") &&
            check(definition->APIVersion == NEXUS_API_VERSION, "Unexpected Nexus API version") &&
            check(std::string{definition->Name} == "GW2 Manny Uploader", "Unexpected addon name") &&
            check(definition->Load != nullptr && definition->Unload != nullptr,
                  "Addon lifecycle callbacks are missing") &&
            check(definition->Provider == UP_GitHub, "Unexpected update provider") && passed;

        AddonAPI_t api{};
        api.ImguiContext = context;
        api.ImguiMalloc = std::bit_cast<void*>(+host_allocate);
        api.ImguiFree = std::bit_cast<void*>(+host_free);
        api.GUI_Register = register_render;
        api.GUI_Deregister = deregister_render;
        api.Log = log_message;
        api.Paths_GetGameDirectory = get_game_directory;
        api.Paths_GetAddonDirectory = get_addon_directory;
        api.InputBinds_RegisterWithString = register_input_bind;
        api.InputBinds_Deregister = deregister_input_bind;
        api.Textures_GetOrCreateFromMemory = create_texture;
        api.QuickAccess_Add = add_quick_access;
        api.QuickAccess_Remove = remove_quick_access;

        definition->Load(&api);
        passed =
            check(registration_count == 2, "Addon did not register both render callbacks") &&
            check(input_registration_count == 1 && registered_input_bind != nullptr,
                  "Addon did not register its window keybind") &&
            check(quick_access_registration_count == 1,
                  "Addon did not register its quick-access shortcut") &&
            check(host_contract_valid, "Addon registered an invalid Nexus resource") &&
            check(lifecycle_events == std::vector<std::string>({"register_main", "register_options",
                                                                "register_input", "create_texture",
                                                                "register_quick_access"}),
                  "Addon resources were not registered in deterministic order") &&
            passed;

        if (registration_count == 2) {
            io.DisplaySize = ImVec2{1280.0F, 720.0F};
            io.DeltaTime = 1.0F / 60.0F;
            ImGui::NewFrame();
            registered_callbacks[0]();
            registered_callbacks[1]();
            ImGui::Render();
        }
        if (registered_input_bind != nullptr) {
            registered_input_bind("KB_GW2_MANNY_UPLOADER_TOGGLE", false);
            registered_input_bind("KB_GW2_MANNY_UPLOADER_TOGGLE", true);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});

        definition->Unload();
        passed =
            check(deregistration_count == 2, "Addon did not deregister both callbacks") &&
            check(input_deregistration_count == 1, "Addon did not deregister its window keybind") &&
            check(quick_access_deregistration_count == 1,
                  "Addon did not remove its quick-access shortcut") &&
            check(host_contract_valid, "Addon deregistered an invalid Nexus resource") && passed;
        if (deregistration_count == 2) {
            passed = check(deregistered_callbacks[0] == registered_callbacks[1],
                           "Options callback was not deregistered first") &&
                     check(deregistered_callbacks[1] == registered_callbacks[0],
                           "Main callback was not deregistered second") &&
                     passed;
        }
        passed = check(lifecycle_events.size() == 9 &&
                           lifecycle_events[5] == "deregister_quick_access" &&
                           lifecycle_events[6] == "deregister_input" &&
                           lifecycle_events[7] == "deregister_options" &&
                           lifecycle_events[8] == "deregister_main",
                       "Addon resources were not released in reverse order") &&
                 passed;
        passed = check(FreeLibrary(module) != 0, "Unable to unload the addon DLL") && passed;
    }
    ImGui::DestroyContext(context);
    std::filesystem::remove_all(root, filesystem_error);
    if (!check(!filesystem_error, "Unable to clean smoke-test directories")) {
        passed = false;
    }

    if (passed) {
        std::cout << "Nexus addon smoke test passed " << hot_load_cycles << " hot-load cycles\n";
        return 0;
    }
    return 1;
}
