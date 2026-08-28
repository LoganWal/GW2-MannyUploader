#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace manny_uploader::addon {

enum class AddonLogLevel : std::uint8_t {
    Critical,
    Warning,
    Info,
    Debug,
};

enum class RenderCallbackKind : std::uint8_t {
    Main,
    Options,
};

using RenderCallback = void (*)();
using InputBindCallback = void (*)(const char* identifier, bool is_release);

struct AddonPaths {
    std::filesystem::path game_directory;
    std::filesystem::path addon_directory;
};

enum class AddonHostErrorCode : std::uint8_t {
    InvalidApi,
    InvalidPath,
    RegistrationFailed,
};

struct AddonHostError {
    AddonHostErrorCode code;
    std::string message;
};

struct QuickAccessStatus;

class IAddonHost {
  public:
    virtual ~IAddonHost() = default;

    [[nodiscard]] virtual std::expected<AddonPaths, AddonHostError> paths() const = 0;
    [[nodiscard]] virtual std::expected<void, AddonHostError>
    register_render(RenderCallbackKind kind, RenderCallback callback) = 0;
    virtual void deregister_render(RenderCallback callback) noexcept = 0;
    [[nodiscard]] virtual std::expected<void, AddonHostError>
    register_input_bind(InputBindCallback callback) = 0;
    virtual void deregister_input_bind() noexcept = 0;
    [[nodiscard]] virtual std::expected<void, AddonHostError>
    register_quick_access_shortcut(const QuickAccessStatus& status) = 0;
    virtual void deregister_quick_access_shortcut() noexcept = 0;
    virtual void log(AddonLogLevel level, std::string_view message) noexcept = 0;
};

enum class AddonRuntimeErrorCode : std::uint8_t {
    InvalidPath,
    InitializationFailed,
};

struct AddonRuntimeError {
    AddonRuntimeErrorCode code;
    std::string message;
};

enum class QuickAccessTint : std::uint8_t {
    Idle,
    Upload,
    Twitch,
};

struct QuickAccessStatus {
    std::string tooltip;
    QuickAccessTint tint{QuickAccessTint::Idle};

    [[nodiscard]] friend bool operator==(const QuickAccessStatus&,
                                         const QuickAccessStatus&) noexcept = default;
};

[[nodiscard]] QuickAccessStatus make_quick_access_status(bool dps_report_enabled,
                                                         bool wingman_enabled, bool donbot_enabled,
                                                         std::string_view donbot_guild,
                                                         std::string_view donbot_discord_route,
                                                         bool twitch_enabled);

class IAddonRuntime {
  public:
    virtual ~IAddonRuntime() = default;

    virtual void render_main() = 0;
    virtual void render_options() = 0;
    virtual void toggle_window() = 0;
    [[nodiscard]] virtual QuickAccessStatus quick_access_status() const = 0;
    virtual void shutdown() noexcept = 0;
};

class IAddonRuntimeFactory {
  public:
    virtual ~IAddonRuntimeFactory() = default;

    [[nodiscard]] virtual std::expected<std::unique_ptr<IAddonRuntime>, AddonRuntimeError>
    create(const AddonPaths& paths) = 0;
};

struct AddonCallbacks {
    RenderCallback main;
    RenderCallback options;
    InputBindCallback input_bind;
};

enum class AddonLifecycleState : std::uint8_t {
    Unloaded,
    Loading,
    Running,
    Unloading,
};

enum class AddonLifecycleErrorCode : std::uint8_t {
    InvalidState,
    InvalidCallback,
    HostFailure,
    RuntimeFailure,
    UnexpectedException,
};

struct AddonLifecycleError {
    AddonLifecycleErrorCode code;
    std::string message;
};

class AddonLifecycle {
  public:
    AddonLifecycle() = default;
    ~AddonLifecycle();

    AddonLifecycle(const AddonLifecycle&) = delete;
    AddonLifecycle& operator=(const AddonLifecycle&) = delete;

    [[nodiscard]] std::expected<void, AddonLifecycleError>
    load(IAddonHost& host, IAddonRuntimeFactory& runtime_factory, AddonCallbacks callbacks);
    void unload() noexcept;

    void render_main() noexcept;
    void render_options() noexcept;
    void process_input_bind(bool is_release) noexcept;
    [[nodiscard]] std::optional<QuickAccessStatus> quick_access_status() noexcept;

    [[nodiscard]] AddonLifecycleState state() const noexcept;
    [[nodiscard]] std::size_t active_callback_count() const noexcept;

  private:
    enum class RuntimeCallbackKind : std::uint8_t {
        MainRender,
        OptionsRender,
        ToggleWindow,
    };

    void invoke(RuntimeCallbackKind kind) noexcept;
    void finish_callback() noexcept;
    void rollback_load() noexcept;

    mutable std::mutex mutex_;
    std::condition_variable callbacks_drained_;
    AddonLifecycleState state_{AddonLifecycleState::Unloaded};
    IAddonHost* host_{};
    AddonCallbacks callbacks_{};
    std::unique_ptr<IAddonRuntime> runtime_;
    std::size_t active_callbacks_{};
    bool main_registered_{};
    bool options_registered_{};
    bool input_bind_registered_{};
    bool quick_access_registered_{};
};

} // namespace manny_uploader::addon
