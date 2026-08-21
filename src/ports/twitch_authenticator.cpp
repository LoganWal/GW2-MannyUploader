#include "manny_uploader/ports/twitch_authenticator.hpp"

namespace manny_uploader::ports {

TwitchAuthenticationOperation
authentication_operation(const TwitchAuthenticationCommand& command) noexcept {
    switch (command.index()) {
    case 0:
        return TwitchAuthenticationOperation::Start;
    case 1:
        return TwitchAuthenticationOperation::Poll;
    case 2:
        return TwitchAuthenticationOperation::Validate;
    case 3:
        return TwitchAuthenticationOperation::Refresh;
    case 4:
        return TwitchAuthenticationOperation::Revoke;
    default:
        return TwitchAuthenticationOperation::Start;
    }
}

} // namespace manny_uploader::ports
