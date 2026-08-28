#include "manny_uploader/addon/addon_lifecycle.hpp"

#include <exception>
#include <utility>

namespace manny_uploader::addon {
namespace {

[[nodiscard]] AddonLifecycleError make_error(AddonLifecycleErrorCode code, std::string message) {
    return AddonLifecycleError{.code = code, .message = std::move(message)};
}

[[nodiscard]] AddonLifecycleError from_host_error(const AddonHostError& error) {
    return make_error(AddonLifecycleErrorCode::HostFailure, error.message);
}

[[nodiscard]] AddonLifecycleError from_runtime_error(const AddonRuntimeError& error) {
    return make_error(AddonLifecycleErrorCode::RuntimeFailure, error.message);
}

} // namespace

QuickAccessStatus make_quick_access_status(bool dps_report_enabled, bool wingman_enabled,
                                           bool donbot_enabled, std::string_view donbot_guild,
                                           std::string_view donbot_discord_route,
                                           bool twitch_enabled) {
    QuickAccessStatus status{.tooltip = "View MannyUploader list", .tint = QuickAccessTint::Idle};
    if (dps_report_enabled) {
        status.tooltip += "\nUploading to dps.report";
        status.tint = QuickAccessTint::Upload;
    }
    if (wingman_enabled) {
        status.tooltip += "\nUploading to GW2Wingman";
        status.tint = QuickAccessTint::Upload;
    }
    if (donbot_enabled) {
        status.tooltip += "\nUploading to DonBot";
        if (!donbot_guild.empty()) {
            status.tooltip += " - ";
            status.tooltip += donbot_guild;
        }
        status.tint = QuickAccessTint::Upload;
    }
    if (!donbot_discord_route.empty()) {
        status.tooltip += "\nPosting DonBot summaries - ";
        status.tooltip += donbot_discord_route;
    }
    if (twitch_enabled) {
        status.tooltip += "\nReporting to Twitch chat";
        status.tint = QuickAccessTint::Twitch;
    }
    return status;
}

AddonLifecycle::~AddonLifecycle() {
    unload();
}

std::expected<void, AddonLifecycleError> AddonLifecycle::load(IAddonHost& host,
                                                              IAddonRuntimeFactory& runtime_factory,
                                                              AddonCallbacks callbacks) {
    if (callbacks.main == nullptr || callbacks.options == nullptr ||
        callbacks.input_bind == nullptr || callbacks.main == callbacks.options) {
        return std::unexpected(make_error(AddonLifecycleErrorCode::InvalidCallback,
                                          "Addon callbacks must be valid and render callbacks must "
                                          "be distinct"));
    }

    {
        const std::scoped_lock lock{mutex_};
        if (state_ != AddonLifecycleState::Unloaded) {
            return std::unexpected(make_error(AddonLifecycleErrorCode::InvalidState,
                                              "Addon is already loading or loaded"));
        }
        state_ = AddonLifecycleState::Loading;
        host_ = &host;
        callbacks_ = callbacks;
    }

    try {
        auto resolved_paths = host.paths();
        if (!resolved_paths) {
            rollback_load();
            return std::unexpected(from_host_error(resolved_paths.error()));
        }

        auto created_runtime = runtime_factory.create(*resolved_paths);
        if (!created_runtime) {
            rollback_load();
            return std::unexpected(from_runtime_error(created_runtime.error()));
        }
        if (*created_runtime == nullptr) {
            rollback_load();
            return std::unexpected(make_error(AddonLifecycleErrorCode::RuntimeFailure,
                                              "Runtime factory returned no runtime"));
        }

        {
            const std::scoped_lock lock{mutex_};
            runtime_ = std::move(*created_runtime);
        }

        auto registered_main = host.register_render(RenderCallbackKind::Main, callbacks.main);
        if (!registered_main) {
            rollback_load();
            return std::unexpected(from_host_error(registered_main.error()));
        }
        {
            const std::scoped_lock lock{mutex_};
            main_registered_ = true;
        }

        auto registered_options =
            host.register_render(RenderCallbackKind::Options, callbacks.options);
        if (!registered_options) {
            rollback_load();
            return std::unexpected(from_host_error(registered_options.error()));
        }

        {
            const std::scoped_lock lock{mutex_};
            options_registered_ = true;
        }

        auto registered_input_bind = host.register_input_bind(callbacks.input_bind);
        if (!registered_input_bind) {
            rollback_load();
            return std::unexpected(from_host_error(registered_input_bind.error()));
        }
        {
            const std::scoped_lock lock{mutex_};
            input_bind_registered_ = true;
        }

        QuickAccessStatus initial_quick_access =
            make_quick_access_status(false, false, false, {}, {}, false);
        {
            const std::scoped_lock lock{mutex_};
            initial_quick_access = runtime_->quick_access_status();
        }
        auto registered_quick_access = host.register_quick_access_shortcut(initial_quick_access);
        {
            const std::scoped_lock lock{mutex_};
            quick_access_registered_ = registered_quick_access.has_value();
            state_ = AddonLifecycleState::Running;
        }
        if (!registered_quick_access) {
            host.log(AddonLogLevel::Warning, registered_quick_access.error().message);
        }
        host.log(AddonLogLevel::Info, "GW2 Manny Uploader loaded");
        return {};
    } catch (const std::exception&) {
        rollback_load();
        return std::unexpected(make_error(AddonLifecycleErrorCode::UnexpectedException,
                                          "Unexpected exception while loading the addon"));
    } catch (...) {
        rollback_load();
        return std::unexpected(make_error(AddonLifecycleErrorCode::UnexpectedException,
                                          "Unknown exception while loading the addon"));
    }
}

void AddonLifecycle::rollback_load() noexcept {
    IAddonHost* host = nullptr;
    AddonCallbacks callbacks{};
    bool main_registered = false;
    bool options_registered = false;
    bool input_bind_registered = false;
    bool quick_access_registered = false;
    std::unique_ptr<IAddonRuntime> runtime;
    {
        const std::scoped_lock lock{mutex_};
        state_ = AddonLifecycleState::Unloading;
        host = host_;
        callbacks = callbacks_;
        main_registered = main_registered_;
        options_registered = options_registered_;
        input_bind_registered = input_bind_registered_;
        quick_access_registered = quick_access_registered_;
    }

    if (host != nullptr) {
        if (quick_access_registered) {
            host->deregister_quick_access_shortcut();
        }
        if (input_bind_registered) {
            host->deregister_input_bind();
        }
        if (options_registered && callbacks.options != nullptr) {
            host->deregister_render(callbacks.options);
        }
        if (main_registered && callbacks.main != nullptr) {
            host->deregister_render(callbacks.main);
        }
    }

    {
        std::unique_lock lock{mutex_};
        callbacks_drained_.wait(lock, [this] { return active_callbacks_ == 0; });
        runtime = std::move(runtime_);
    }
    if (runtime != nullptr) {
        runtime->shutdown();
    }
    {
        const std::scoped_lock lock{mutex_};
        host_ = nullptr;
        callbacks_ = {};
        main_registered_ = false;
        options_registered_ = false;
        input_bind_registered_ = false;
        quick_access_registered_ = false;
        state_ = AddonLifecycleState::Unloaded;
    }
}

void AddonLifecycle::unload() noexcept {
    IAddonHost* host = nullptr;
    AddonCallbacks callbacks{};
    bool main_registered = false;
    bool options_registered = false;
    bool input_bind_registered = false;
    bool quick_access_registered = false;
    {
        const std::scoped_lock lock{mutex_};
        if (state_ == AddonLifecycleState::Unloaded || state_ == AddonLifecycleState::Unloading) {
            return;
        }
        state_ = AddonLifecycleState::Unloading;
        host = host_;
        callbacks = callbacks_;
        main_registered = main_registered_;
        options_registered = options_registered_;
        input_bind_registered = input_bind_registered_;
        quick_access_registered = quick_access_registered_;
    }

    if (host != nullptr) {
        if (quick_access_registered) {
            host->deregister_quick_access_shortcut();
        }
        if (input_bind_registered) {
            host->deregister_input_bind();
        }
        if (options_registered && callbacks.options != nullptr) {
            host->deregister_render(callbacks.options);
        }
        if (main_registered && callbacks.main != nullptr) {
            host->deregister_render(callbacks.main);
        }
    }

    std::unique_ptr<IAddonRuntime> runtime;
    {
        std::unique_lock lock{mutex_};
        callbacks_drained_.wait(lock, [this] { return active_callbacks_ == 0; });
        runtime = std::move(runtime_);
    }
    if (runtime != nullptr) {
        runtime->shutdown();
    }

    if (host != nullptr) {
        host->log(AddonLogLevel::Info, "GW2 Manny Uploader unloaded");
    }
    {
        const std::scoped_lock lock{mutex_};
        host_ = nullptr;
        callbacks_ = {};
        main_registered_ = false;
        options_registered_ = false;
        input_bind_registered_ = false;
        quick_access_registered_ = false;
        state_ = AddonLifecycleState::Unloaded;
    }
}

void AddonLifecycle::render_main() noexcept {
    invoke(RuntimeCallbackKind::MainRender);
}

void AddonLifecycle::render_options() noexcept {
    invoke(RuntimeCallbackKind::OptionsRender);
}

void AddonLifecycle::process_input_bind(bool is_release) noexcept {
    if (!is_release) {
        invoke(RuntimeCallbackKind::ToggleWindow);
    }
}

std::optional<QuickAccessStatus> AddonLifecycle::quick_access_status() noexcept {
    IAddonRuntime* runtime = nullptr;
    {
        const std::scoped_lock lock{mutex_};
        if (state_ != AddonLifecycleState::Running || runtime_ == nullptr) {
            return std::nullopt;
        }
        ++active_callbacks_;
        runtime = runtime_.get();
    }
    std::optional<QuickAccessStatus> status;
    try {
        status = runtime->quick_access_status();
    } catch (...) {
    }
    finish_callback();
    return status;
}

void AddonLifecycle::invoke(RuntimeCallbackKind kind) noexcept {
    IAddonRuntime* runtime = nullptr;
    IAddonHost* host = nullptr;
    {
        const std::scoped_lock lock{mutex_};
        if (state_ != AddonLifecycleState::Running || runtime_ == nullptr) {
            return;
        }
        ++active_callbacks_;
        runtime = runtime_.get();
        host = host_;
    }

    try {
        if (kind == RuntimeCallbackKind::MainRender) {
            runtime->render_main();
        } else if (kind == RuntimeCallbackKind::OptionsRender) {
            runtime->render_options();
        } else {
            runtime->toggle_window();
        }
    } catch (const std::exception&) {
        if (host != nullptr) {
            host->log(AddonLogLevel::Critical,
                      "An exception was contained at a Nexus callback boundary");
        }
    } catch (...) {
        if (host != nullptr) {
            host->log(AddonLogLevel::Critical,
                      "An unknown exception was contained at a Nexus callback boundary");
        }
    }
    finish_callback();
}

void AddonLifecycle::finish_callback() noexcept {
    const std::scoped_lock lock{mutex_};
    --active_callbacks_;
    if (active_callbacks_ == 0) {
        callbacks_drained_.notify_all();
    }
}

AddonLifecycleState AddonLifecycle::state() const noexcept {
    const std::scoped_lock lock{mutex_};
    return state_;
}

std::size_t AddonLifecycle::active_callback_count() const noexcept {
    const std::scoped_lock lock{mutex_};
    return active_callbacks_;
}

} // namespace manny_uploader::addon
