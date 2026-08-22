#include "manny_uploader/filesystem/change_notifying_log_candidate_source.hpp"

#include <windows.h>

#include <expected>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>
#include <system_error>
#include <utility>

namespace manny_uploader::filesystem {
namespace {

[[nodiscard]] bool path_is_unavailable(DWORD error) noexcept {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

[[nodiscard]] ports::LogCandidateSourceError monitor_error(std::string message) {
    return ports::LogCandidateSourceError{
        .code = ports::LogCandidateSourceErrorCode::ScanFailed,
        .message = std::move(message),
    };
}

class WindowsDirectoryChangeMonitor final : public IDirectoryChangeMonitor {
  public:
    ~WindowsDirectoryChangeMonitor() override {
        close();
    }

    [[nodiscard]] std::expected<void, ports::LogCandidateSourceError>
    reconfigure(const std::filesystem::path& root, bool recursive) override {
        if (root.empty()) {
            return std::unexpected(ports::LogCandidateSourceError{
                .code = ports::LogCandidateSourceErrorCode::InvalidConfiguration,
                .message = "Log directory path must not be empty",
            });
        }

        std::error_code error;
        auto absolute_root = std::filesystem::absolute(root, error);
        if (error) {
            return std::unexpected(ports::LogCandidateSourceError{
                .code = ports::LogCandidateSourceErrorCode::InvalidConfiguration,
                .message = "Unable to resolve log directory for native notifications",
            });
        }

        close();
        root_ = absolute_root.lexically_normal();
        recursive_ = recursive;
        return {};
    }

    [[nodiscard]] std::expected<DirectoryChangeStatus, ports::LogCandidateSourceError>
    poll(const std::stop_token& stop_token) override {
        if (stop_token.stop_requested()) {
            return std::unexpected(ports::LogCandidateSourceError{
                .code = ports::LogCandidateSourceErrorCode::Cancelled,
                .message = "Log directory monitoring cancelled",
            });
        }

        if (handle_ == INVALID_HANDLE_VALUE) {
            const auto attributes = GetFileAttributesW(root_.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                const auto error = GetLastError();
                if (path_is_unavailable(error)) {
                    return DirectoryChangeStatus::Unavailable;
                }
                return std::unexpected(
                    monitor_error("Unable to inspect the native log notification directory"));
            }
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                return std::unexpected(
                    monitor_error("Native log notification path is not a directory"));
            }

            handle_ = FindFirstChangeNotificationW(
                root_.c_str(), recursive_ ? TRUE : FALSE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                    FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE |
                    FILE_NOTIFY_CHANGE_CREATION);
            if (handle_ == INVALID_HANDLE_VALUE) {
                const auto error = GetLastError();
                if (path_is_unavailable(error)) {
                    return DirectoryChangeStatus::Unavailable;
                }
                return std::unexpected(
                    monitor_error("Unable to start native log-directory notifications"));
            }
            return DirectoryChangeStatus::Changed;
        }

        const auto wait_result = WaitForSingleObject(handle_, 0);
        if (wait_result == WAIT_TIMEOUT) {
            return DirectoryChangeStatus::Unchanged;
        }
        if (wait_result == WAIT_OBJECT_0) {
            if (FindNextChangeNotification(handle_) == FALSE) {
                close();
                return std::unexpected(
                    monitor_error("Unable to rearm native log-directory notifications"));
            }
            return DirectoryChangeStatus::Changed;
        }

        close();
        return std::unexpected(monitor_error("Unable to query native log-directory notifications"));
    }

  private:
    void close() noexcept {
        if (handle_ != INVALID_HANDLE_VALUE) {
            static_cast<void>(FindCloseChangeNotification(handle_));
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

    std::filesystem::path root_;
    HANDLE handle_{INVALID_HANDLE_VALUE};
    bool recursive_{};
};

} // namespace

std::unique_ptr<IDirectoryChangeMonitor> make_windows_directory_change_monitor() {
    return std::make_unique<WindowsDirectoryChangeMonitor>();
}

} // namespace manny_uploader::filesystem
