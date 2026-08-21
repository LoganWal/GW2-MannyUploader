#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
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

class IAddonHost {
  public:
    virtual ~IAddonHost() = default;

    [[nodiscard]] virtual std::expected<AddonPaths, AddonHostError> paths() const = 0;
    [[nodiscard]] virtual std::expected<void, AddonHostError>
    register_render(RenderCallbackKind kind, RenderCallback callback) = 0;
    virtual void deregister_render(RenderCallback callback) noexcept = 0;
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

class IAddonRuntime {
  public:
    virtual ~IAddonRuntime() = default;

    virtual void render_main() = 0;
    virtual void render_options() = 0;
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

    [[nodiscard]] AddonLifecycleState state() const noexcept;
    [[nodiscard]] std::size_t active_callback_count() const noexcept;

  private:
    void render(RenderCallbackKind kind) noexcept;
    void finish_callback() noexcept;
    void rollback_load(bool main_registered, bool options_registered) noexcept;

    mutable std::mutex mutex_;
    std::condition_variable callbacks_drained_;
    AddonLifecycleState state_{AddonLifecycleState::Unloaded};
    IAddonHost* host_{};
    AddonCallbacks callbacks_{};
    std::unique_ptr<IAddonRuntime> runtime_;
    std::size_t active_callbacks_{};
    bool main_registered_{};
    bool options_registered_{};
};

} // namespace manny_uploader::addon
