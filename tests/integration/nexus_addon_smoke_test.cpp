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

namespace {

using GetAddonDefinition = AddonDefinition_t* (*)();

std::array<GUI_RENDER, 2> registered_callbacks{};
std::array<GUI_RENDER, 2> deregistered_callbacks{};
std::size_t registration_count{};
std::size_t deregistration_count{};
std::string game_directory;
std::string addon_directory;

void* host_allocate(std::size_t size, void*) {
    return std::malloc(size);
}

void host_free(void* memory, void*) {
    std::free(memory);
}

void register_render(ERenderType, GUI_RENDER callback) {
    if (registration_count < registered_callbacks.size()) {
        registered_callbacks[registration_count++] = callback;
    }
}

void deregister_render(GUI_RENDER callback) {
    if (deregistration_count < deregistered_callbacks.size()) {
        deregistered_callbacks[deregistration_count++] = callback;
    }
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

    HMODULE module = LoadLibraryA(arguments[1]);
    if (!check(module != nullptr, "Unable to load the addon DLL")) {
        ImGui::DestroyContext(context);
        std::filesystem::remove_all(root, filesystem_error);
        return 1;
    }
    const auto get_definition =
        std::bit_cast<GetAddonDefinition>(GetProcAddress(module, "GetAddonDef"));
    if (!check(get_definition != nullptr, "GetAddonDef is not exported")) {
        FreeLibrary(module);
        ImGui::DestroyContext(context);
        std::filesystem::remove_all(root, filesystem_error);
        return 1;
    }

    AddonDefinition_t* definition = get_definition();
    bool passed =
        check(definition != nullptr, "GetAddonDef returned null") &&
        check(definition->APIVersion == NEXUS_API_VERSION, "Unexpected Nexus API version") &&
        check(std::string{definition->Name} == "GW2 Manny Uploader", "Unexpected addon name") &&
        check(definition->Load != nullptr && definition->Unload != nullptr,
              "Addon lifecycle callbacks are missing") &&
        check(definition->Provider == UP_GitHub, "Unexpected update provider");

    AddonAPI_t api{};
    api.ImguiContext = context;
    api.ImguiMalloc = std::bit_cast<void*>(+host_allocate);
    api.ImguiFree = std::bit_cast<void*>(+host_free);
    api.GUI_Register = register_render;
    api.GUI_Deregister = deregister_render;
    api.Log = log_message;
    api.Paths_GetGameDirectory = get_game_directory;
    api.Paths_GetAddonDirectory = get_addon_directory;

    definition->Load(&api);
    passed =
        check(registration_count == 2, "Addon did not register both render callbacks") && passed;

    if (registration_count == 2) {
        io.DisplaySize = ImVec2{1280.0F, 720.0F};
        io.DeltaTime = 1.0F / 60.0F;
        ImGui::NewFrame();
        registered_callbacks[0]();
        registered_callbacks[1]();
        ImGui::Render();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{100});

    definition->Unload();
    passed = check(deregistration_count == 2, "Addon did not deregister both callbacks") && passed;
    if (deregistration_count == 2) {
        passed = check(deregistered_callbacks[0] == registered_callbacks[1],
                       "Options callback was not deregistered first") &&
                 check(deregistered_callbacks[1] == registered_callbacks[0],
                       "Main callback was not deregistered second") &&
                 passed;
    }

    passed = check(FreeLibrary(module) != 0, "Unable to unload the addon DLL") && passed;
    ImGui::DestroyContext(context);
    std::filesystem::remove_all(root, filesystem_error);
    if (!check(!filesystem_error, "Unable to clean smoke-test directories")) {
        passed = false;
    }

    if (passed) {
        std::cout << "Nexus addon smoke test passed\n";
        return 0;
    }
    return 1;
}
