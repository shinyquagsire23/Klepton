#include "Models/UserDetail.hpp"
#include "Utils.hpp"

namespace BeatSaver::Models {
    void UserDetail::GetAvatarImageAsync(std::function<void(std::optional<std::vector<uint8_t>>)> onFinished) const {
        if (!onFinished) return;

        std::thread([](std::string avatarURL, std::function<void(std::optional<std::vector<uint8_t>>)> onFinished){
            onFinished(Utils::GetData(avatarURL));
        }, AvatarURL, std::forward<std::function<void(std::optional<std::vector<uint8_t>>)>>(onFinished)).detach();
    }

    std::optional<std::vector<uint8_t>> UserDetail::GetAvatarImage() const {
        return Utils::GetData(AvatarURL);
    }
}
