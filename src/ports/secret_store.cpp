#include "manny_uploader/ports/secret_store.hpp"

namespace manny_uploader::ports {

bool is_known_secret_id(SecretId id) noexcept {
    switch (id) {
    case SecretId::DpsReportUserToken:
    case SecretId::DonBotGw2ApiKey:
    case SecretId::TwitchOAuthSession:
        return true;
    }
    return false;
}

std::string_view secret_id_name(SecretId id) noexcept {
    switch (id) {
    case SecretId::DpsReportUserToken:
        return "dps.report user token";
    case SecretId::DonBotGw2ApiKey:
        return "DonBot Guild Wars 2 API key";
    case SecretId::TwitchOAuthSession:
        return "Twitch OAuth session";
    }
    return "unknown credential";
}

} // namespace manny_uploader::ports
