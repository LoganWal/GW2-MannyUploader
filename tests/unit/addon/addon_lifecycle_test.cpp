#include "manny_uploader/addon/addon_lifecycle.hpp"
#include "support/test_suite.hpp"

#include <condition_variable>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace manny_uploader::test {
namespace {

using addon::AddonCallbacks;
using addon::AddonHostError;
using addon::AddonHostErrorCode;
using addon::AddonLifecycle;
using addon::AddonLifecycleErrorCode;
using addon::AddonLifecycleState;
using addon::AddonLogLevel;
using addon::AddonPaths;
using addon::AddonRuntimeError;
using addon::IAddonHost;
using addon::IAddonRuntime;
using addon::IAddonRuntimeFactory;
using addon::RenderCallback;
using addon::RenderCallbackKind;

void main_callback() {}
void options_callback() {}

struct RuntimeState {
    std::mutex mutex;
    std::condition_variable condition;
    std::size_t main_renders{};
    std::size_t options_renders{};
    std::size_t shutdowns{};
    bool block_main{};
    bool main_entered{};
    bool release_main{};
    bool throw_main{};
};

class FakeRuntime final : public IAddonRuntime {
  public:
    explicit FakeRuntime(std::shared_ptr<RuntimeState> state) : state_{std::move(state)} {}

    void render_main() override {
        std::unique_lock lock{state_->mutex};
        ++state_->main_renders;
        state_->main_entered = true;
        state_->condition.notify_all();
        state_->condition.wait(lock,
                               [this] { return !state_->block_main || state_->release_main; });
        if (state_->throw_main) {
            throw std::runtime_error{"render failed"};
        }
    }

    void render_options() override {
        const std::scoped_lock lock{state_->mutex};
        ++state_->options_renders;
    }

    void shutdown() noexcept override {
        const std::scoped_lock lock{state_->mutex};
        ++state_->shutdowns;
        state_->condition.notify_all();
    }

  private:
    std::shared_ptr<RuntimeState> state_;
};

class FakeRuntimeFactory final : public IAddonRuntimeFactory {
  public:
    [[nodiscard]] std::expected<std::unique_ptr<IAddonRuntime>, AddonRuntimeError>
    create(const AddonPaths& paths) override {
        received_paths = paths;
        if (throw_on_create) {
            throw std::runtime_error{"factory exception"};
        }
        if (fail) {
            return std::unexpected(AddonRuntimeError{
                .code = addon::AddonRuntimeErrorCode::InitializationFailed,
                .message = "runtime creation failed",
            });
        }
        return std::make_unique<FakeRuntime>(state);
    }

    std::shared_ptr<RuntimeState> state{std::make_shared<RuntimeState>()};
    AddonPaths received_paths;
    bool fail{};
    bool throw_on_create{};
};

class FakeHost final : public IAddonHost {
  public:
    [[nodiscard]] std::expected<AddonPaths, AddonHostError> paths() const override {
        if (fail_paths) {
            return std::unexpected(AddonHostError{
                .code = AddonHostErrorCode::InvalidPath,
                .message = "host paths failed",
            });
        }
        return configured_paths;
    }

    [[nodiscard]] std::expected<void, AddonHostError>
    register_render(RenderCallbackKind kind, RenderCallback callback) override {
        if (kind == RenderCallbackKind::Options && fail_options_registration) {
            return std::unexpected(AddonHostError{
                .code = AddonHostErrorCode::RegistrationFailed,
                .message = "options registration failed",
            });
        }
        registrations.emplace_back(kind, callback);
        return {};
    }

    void deregister_render(RenderCallback callback) noexcept override {
        deregistrations.push_back(callback);
    }

    void log(AddonLogLevel level, std::string_view message) noexcept override {
        logs.emplace_back(level, message);
    }

    AddonPaths configured_paths{
        .game_directory = std::filesystem::path{"C:/Games/Guild Wars 2"},
        .addon_directory = std::filesystem::path{"C:/Games/Guild Wars 2/addons/GW2MannyUploader"},
    };
    bool fail_paths{};
    bool fail_options_registration{};
    std::vector<std::pair<RenderCallbackKind, RenderCallback>> registrations;
    std::vector<RenderCallback> deregistrations;
    std::vector<std::pair<AddonLogLevel, std::string>> logs;
};

[[nodiscard]] AddonCallbacks callbacks() {
    return AddonCallbacks{.main = main_callback, .options = options_callback};
}

void successful_lifecycle_tests(TestSuite& suite) {
    FakeHost host;
    FakeRuntimeFactory factory;
    AddonLifecycle lifecycle;

    const auto loaded = lifecycle.load(host, factory, callbacks());
    MANNY_CHECK(suite, loaded.has_value());
    MANNY_CHECK(suite, lifecycle.state() == AddonLifecycleState::Running);
    MANNY_CHECK(suite, host.registrations.size() == 2);
    MANNY_CHECK(suite, host.registrations[0].first == RenderCallbackKind::Main);
    MANNY_CHECK(suite, host.registrations[1].first == RenderCallbackKind::Options);
    MANNY_CHECK(suite,
                factory.received_paths.addon_directory == host.configured_paths.addon_directory);

    lifecycle.render_main();
    lifecycle.render_options();
    {
        const std::scoped_lock lock{factory.state->mutex};
        MANNY_CHECK(suite, factory.state->main_renders == 1);
        MANNY_CHECK(suite, factory.state->options_renders == 1);
    }

    lifecycle.unload();
    MANNY_CHECK(suite, lifecycle.state() == AddonLifecycleState::Unloaded);
    MANNY_CHECK(suite, host.deregistrations.size() == 2);
    MANNY_CHECK(suite, host.deregistrations[0] == options_callback);
    MANNY_CHECK(suite, host.deregistrations[1] == main_callback);
    {
        const std::scoped_lock lock{factory.state->mutex};
        MANNY_CHECK(suite, factory.state->shutdowns == 1);
    }

    lifecycle.render_main();
    lifecycle.unload();
    MANNY_CHECK(suite, lifecycle.active_callback_count() == 0);
}

void validation_and_rollback_tests(TestSuite& suite) {
    FakeHost host;
    FakeRuntimeFactory factory;
    AddonLifecycle lifecycle;

    const auto invalid = lifecycle.load(host, factory, {.main = main_callback, .options = nullptr});
    MANNY_CHECK(suite, !invalid.has_value());
    MANNY_CHECK(suite, invalid.error().code == AddonLifecycleErrorCode::InvalidCallback);
    MANNY_CHECK(suite, lifecycle.state() == AddonLifecycleState::Unloaded);

    host.fail_options_registration = true;
    const auto partial = lifecycle.load(host, factory, callbacks());
    MANNY_CHECK(suite, !partial.has_value());
    MANNY_CHECK(suite, partial.error().code == AddonLifecycleErrorCode::HostFailure);
    MANNY_CHECK(suite, lifecycle.state() == AddonLifecycleState::Unloaded);
    MANNY_CHECK(suite, host.deregistrations.size() == 1);
    MANNY_CHECK(suite, host.deregistrations.front() == main_callback);
    {
        const std::scoped_lock lock{factory.state->mutex};
        MANNY_CHECK(suite, factory.state->shutdowns == 1);
    }

    host.fail_options_registration = false;
    factory.fail = true;
    const auto runtime_failure = lifecycle.load(host, factory, callbacks());
    MANNY_CHECK(suite, !runtime_failure.has_value());
    MANNY_CHECK(suite, runtime_failure.error().code == AddonLifecycleErrorCode::RuntimeFailure);
    MANNY_CHECK(suite, lifecycle.state() == AddonLifecycleState::Unloaded);

    factory.fail = false;
    factory.throw_on_create = true;
    const auto exception = lifecycle.load(host, factory, callbacks());
    MANNY_CHECK(suite, !exception.has_value());
    MANNY_CHECK(suite, exception.error().code == AddonLifecycleErrorCode::UnexpectedException);
    MANNY_CHECK(suite, lifecycle.state() == AddonLifecycleState::Unloaded);
}

void exception_boundary_tests(TestSuite& suite) {
    FakeHost host;
    FakeRuntimeFactory factory;
    factory.state->throw_main = true;
    AddonLifecycle lifecycle;
    MANNY_CHECK(suite, lifecycle.load(host, factory, callbacks()).has_value());

    lifecycle.render_main();
    MANNY_CHECK(suite, lifecycle.active_callback_count() == 0);
    MANNY_CHECK(suite, host.logs.size() == 2);
    MANNY_CHECK(suite, host.logs.back().first == AddonLogLevel::Critical);
    lifecycle.unload();
}

void unload_waits_for_callback_tests(TestSuite& suite) {
    FakeHost host;
    FakeRuntimeFactory factory;
    factory.state->block_main = true;
    AddonLifecycle lifecycle;
    MANNY_CHECK(suite, lifecycle.load(host, factory, callbacks()).has_value());

    std::jthread render_thread{[&lifecycle] { lifecycle.render_main(); }};
    {
        std::unique_lock lock{factory.state->mutex};
        factory.state->condition.wait(lock, [&factory] { return factory.state->main_entered; });
    }
    MANNY_CHECK(suite, lifecycle.active_callback_count() == 1);

    std::jthread unload_thread{[&lifecycle] { lifecycle.unload(); }};
    for (std::size_t attempts = 0;
         attempts < 1'000'000 && lifecycle.state() != AddonLifecycleState::Unloading; ++attempts) {
        std::this_thread::yield();
    }
    MANNY_CHECK(suite, lifecycle.state() == AddonLifecycleState::Unloading);
    {
        const std::scoped_lock lock{factory.state->mutex};
        MANNY_CHECK(suite, factory.state->shutdowns == 0);
        factory.state->release_main = true;
        factory.state->condition.notify_all();
    }
    render_thread.join();
    unload_thread.join();

    MANNY_CHECK(suite, lifecycle.state() == AddonLifecycleState::Unloaded);
    {
        const std::scoped_lock lock{factory.state->mutex};
        MANNY_CHECK(suite, factory.state->shutdowns == 1);
    }
}

} // namespace

void run_addon_lifecycle_tests(TestSuite& suite) {
    successful_lifecycle_tests(suite);
    validation_and_rollback_tests(suite);
    exception_boundary_tests(suite);
    unload_waits_for_callback_tests(suite);
}

} // namespace manny_uploader::test
