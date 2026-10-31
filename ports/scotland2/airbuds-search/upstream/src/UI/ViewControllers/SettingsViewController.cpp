#include <cctype>

#include "HMUI/CurvedTextMeshPro.hpp"
#include "HMUI/Touchable.hpp"
#include "beatsaber-hook/shared/utils/il2cpp-utils.hpp"
#include "bsml/shared/BSML/Components/ExternalComponents.hpp"
#include <bsml/shared/BSML/Components/ButtonIconImage.hpp>
#include <UnityEngine/Mesh.hpp>
#include <UnityEngine/Resources.hpp>
#include <VRUIControls/VRGraphicRaycaster.hpp>
#include <bsml/shared/BSML.hpp>
#include <bsml/shared/BSML/Tags/ModalTag.hpp>
#include <bsml/shared/Helpers/delegates.hpp>
#include <bsml/shared/Helpers/getters.hpp>
#include <scotland2/shared/loader.hpp>
#include <web-utils/shared/WebUtils.hpp>
#include <fstream>

#include "Configuration.hpp"
#include "SpriteCache.hpp"
#include "Utils.hpp"
#include "assets.hpp"
#include "UI/FlowCoordinators/AirbudsSearchFlowCoordinator.hpp"
#include "UI/ViewControllers/SettingsViewController.hpp"
#include "main.hpp"

DEFINE_TYPE(AirbudsSearch::UI::ViewControllers, SettingsViewController);

using namespace AirbudsSearch::UI::ViewControllers;

namespace {

std::string trimAscii(const std::string& text) {
    size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) {
        ++start;
    }
    if (start >= text.size()) {
        return "";
    }
    size_t end = text.size() - 1;
    while (end > start && std::isspace(static_cast<unsigned char>(text[end]))) {
        --end;
    }
    return text.substr(start, end - start + 1);
}

constexpr std::string_view FRIEND_HISTORY_CACHE_DIR = "friend_recently_played";

}

void SettingsViewController::DidActivate(const bool isFirstActivation, bool addedToHierarchy, bool screenSystemDisabling) {
    if (isFirstActivation) {
        // Initialize dropdown options
        historyClearRangeOptions_ = ListW<StringW>::New();
        historyClearRangeOptions_->Add("Older than 1 day");
        historyClearRangeOptions_->Add("Older than 1 week");
        historyClearRangeOptions_->Add("Older than 1 month");
        historyClearRangeOptions_->Add("All entries");
        historyClearRangeValue_ = "Older than 1 week";

        friendHistoryClearRangeOptions_ = ListW<StringW>::New();
        friendHistoryClearRangeOptions_->Add("Older than 1 day");
        friendHistoryClearRangeOptions_->Add("Older than 1 week");
        friendHistoryClearRangeOptions_->Add("Older than 1 month");
        friendHistoryClearRangeOptions_->Add("All entries");
        friendHistoryClearRangeValue_ = "Older than 1 week";

        BSML::parse_and_construct(IncludedAssets::SettingsViewController_bsml, this->get_transform(), this);

        isClearingCache_ = false;
        isClearingHistory_ = false;
        isClearingFriendHistory_ = false;

#if HOT_RELOAD
        fileWatcher->filePath = "/sdcard/SettingsViewController.bsml";
        fileWatcher->checkInterval = 1.0f;
#endif
        modalView_ = get_gameObject()->AddComponent<ModalView*>();
    } else {
        refresh();
    }
}

void SettingsViewController::PostParse() {
    static constexpr std::string_view KEY_CLIPBOARD_ICON = "clipboard-icon";
    UnityW<UnityEngine::Sprite> sprite = SpriteCache::getInstance().get(KEY_CLIPBOARD_ICON);
    if (!sprite) {
        sprite = BSML::Lite::ArrayToSprite(IncludedAssets::clipboard_icon_png);
        SpriteCache::getInstance().add(KEY_CLIPBOARD_ICON, sprite);
    }
    refreshTokenPasteButton_->GetComponent<BSML::ButtonIconImage*>()->SetIcon(sprite);
    static constexpr float scale = 1.5f;
    refreshTokenPasteButton_->get_transform()->Find("Content/Icon")->set_localScale({scale, scale, scale});
    Utils::removeRaycastFromButtonIcon(refreshTokenPasteButton_);

    refresh();
}

void SettingsViewController::refresh() {
    refreshAirbudsTokenStatus();
    refreshKakasiStatus();
    refreshCacheSizeStatus();
    refreshHistoryCacheSizeStatus();
    refreshFriendHistoryCacheSizeStatus();
}

void SettingsViewController::refreshAirbudsTokenStatus() {
    const std::string token = AirbudsSearch::getAirbudsRefreshToken();
    refreshTokenTextField_->set_text(token);
    if (token.empty()) {
        refreshTokenStatusTextView_->set_text("No refresh token saved.");
    } else {
        refreshTokenStatusTextView_->set_text("Refresh token saved.");
    }
}

void SettingsViewController::refreshCacheSizeStatus() {
    cacheSizeTextView_->set_text("(Calculating Usage...)");
    std::thread([this]() {
        const uintmax_t cacheSizeInBytes = getDirectorySizeInBytes(AirbudsSearch::getDataDirectory() / "cache");
        BSML::MainThreadScheduler::Schedule([this, cacheSizeInBytes]() {
            cacheSizeTextView_->set_text(std::format("({} Used)", getHumanReadableSize(cacheSizeInBytes)));

            if (isClearingCache_) {
                clearCacheButton_->set_interactable(false);
                clearCacheButton_->GetComponentInChildren<HMUI::CurvedTextMeshPro*>()->set_text("Clearing...");
            } else {
                clearCacheButton_->set_interactable(true);
                clearCacheButton_->GetComponentInChildren<HMUI::CurvedTextMeshPro*>()->set_text("Clear Cache");
            }
        });
    }).detach();
}

void SettingsViewController::onPasteRefreshTokenButtonClicked() {
    static auto UnityEngine_GUIUtility_get_systemCopyBuffer = il2cpp_utils::resolve_icall<StringW>("UnityEngine.GUIUtility::get_systemCopyBuffer");
    const std::string text = UnityEngine_GUIUtility_get_systemCopyBuffer();
    refreshTokenTextField_->set_text(text);
}

void SettingsViewController::onClearRefreshTokenButtonClicked() {
    refreshTokenTextField_->set_text("");
    AirbudsSearch::clearAirbudsRefreshToken();
    if (AirbudsSearch::airbudsClient) {
        AirbudsSearch::airbudsClient->resetAirbudsCredentials();
    }
    refreshAirbudsTokenStatus();
    modalView_->setMessage("Refresh token cleared.");
    modalView_->setPrimaryButton(false, "", nullptr);
    modalView_->setSecondaryButton(true, "OK", nullptr);
    modalView_->show();
}

void SettingsViewController::onSaveRefreshTokenButtonClicked() {
    const std::string trimmed = trimAscii(refreshTokenTextField_->get_text());
    if (trimmed.empty()) {
        refreshTokenTextField_->set_text("");
        AirbudsSearch::clearAirbudsRefreshToken();
        if (AirbudsSearch::airbudsClient) {
            AirbudsSearch::airbudsClient->resetAirbudsCredentials();
        }
        refreshAirbudsTokenStatus();
        modalView_->setMessage("Refresh token cleared.");
        modalView_->setPrimaryButton(false, "", nullptr);
        modalView_->setSecondaryButton(true, "OK", nullptr);
        modalView_->show();
        return;
    }

    AirbudsSearch::setAirbudsRefreshToken(trimmed);
    if (AirbudsSearch::airbudsClient) {
        AirbudsSearch::airbudsClient->resetAirbudsCredentials();
    }
    refreshAirbudsTokenStatus();
    modalView_->setMessage("Refresh token saved.");
    modalView_->setPrimaryButton(false, "", nullptr);
    modalView_->setSecondaryButton(true, "OK", nullptr);
    modalView_->show();

    if (AirbudsSearch::hasAirbudsRefreshToken()) {
        AirbudsSearch::UI::FlowCoordinators::AirbudsSearchFlowCoordinator::reopen();
    }
}

void SettingsViewController::onClearCacheButtonClicked() {
    if (isClearingCache_) {
        return;
    }
    isClearingCache_ = true;
    clearCacheButton_->set_interactable(false);
    cacheSizeTextView_->set_text("(Clearing...)");

    std::thread([this]() {
        std::filesystem::remove_all(AirbudsSearch::getDataDirectory() / "cache");
        isClearingCache_ = false;
        BSML::MainThreadScheduler::Schedule([this]() {
            clearCacheButton_->set_interactable(true);
            refreshCacheSizeStatus();
        });
    }).detach();
}

uintmax_t SettingsViewController::getDirectorySizeInBytes(const std::filesystem::path& path) {
    uintmax_t sizeInBytes = 0;
    if (!std::filesystem::exists(path)) {
        return 0;
    }
    for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(path)) {
        if (std::filesystem::is_regular_file(entry)) {
            sizeInBytes += std::filesystem::file_size(entry.path());
        }
    }
    return sizeInBytes;
}

std::string SettingsViewController::getHumanReadableSize(const uintmax_t bytes) {
    if (bytes == 0) {
        return "0 B";
    }
    constexpr const char* const suffixes[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    size_t i = 0;
    double size = static_cast<double>(bytes);
    while (size >= 1024 && i < sizeof(suffixes)) {
        size /= 1024;
        ++i;
    }
    return std::format("{:.1f} {}", size, suffixes[i]);
}

void SettingsViewController::refreshKakasiStatus() {
    CModInfo modInfo{"airbuds-search-kakasi", "0.0.0", 0};
    CModResult mod = modloader_get_mod(&modInfo, MatchType_IdOnly);
    if (mod.handle) {
        kakasiStatusTextView_->set_text("<color=green>(Loaded)</color>");
    } else {
        kakasiStatusTextView_->set_text("<color=yellow>(Not Installed)</color>");
    }
}

void SettingsViewController::refreshHistoryCacheSizeStatus() {
    historyCacheSizeTextView_->set_text("(Calculating...)");
    std::thread([this]() {
        const std::filesystem::path cachePath = AirbudsSearch::getDataDirectory() / "recently_played_cache.json";
        uintmax_t sizeInBytes = 0;
        size_t entryCount = 0;

        if (std::filesystem::exists(cachePath)) {
            sizeInBytes = std::filesystem::file_size(cachePath);

            std::ifstream file(cachePath, std::ios::binary);
            if (file.is_open()) {
                std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                rapidjson::Document doc;
                doc.Parse(data.c_str());
                if (doc.IsObject() && doc.HasMember("tracks") && doc["tracks"].IsArray()) {
                    entryCount = doc["tracks"].GetArray().Size();
                }
            }
        }

        BSML::MainThreadScheduler::Schedule([this, sizeInBytes, entryCount]() {
            historyCacheSizeTextView_->set_text(std::format("({} entries, {})", entryCount, getHumanReadableSize(sizeInBytes)));

            const bool clearing = isClearingHistory_.load();
            clearHistoryButton_->set_interactable(!clearing);
        });
    }).detach();
}

void SettingsViewController::refreshFriendHistoryCacheSizeStatus() {
    friendHistoryCacheSizeTextView_->set_text("(Calculating...)");
    std::thread([this]() {
        const std::filesystem::path cacheDir = AirbudsSearch::getDataDirectory() / std::string(FRIEND_HISTORY_CACHE_DIR);
        uintmax_t sizeInBytes = 0;
        size_t entryCount = 0;

        if (std::filesystem::exists(cacheDir)) {
            for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(cacheDir)) {
                if (!entry.is_regular_file()) {
                    continue;
                }
                sizeInBytes += std::filesystem::file_size(entry.path());

                std::ifstream file(entry.path(), std::ios::binary);
                if (!file.is_open()) {
                    continue;
                }
                std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                rapidjson::Document doc;
                doc.Parse(data.c_str());
                if (doc.IsObject() && doc.HasMember("tracks") && doc["tracks"].IsArray()) {
                    entryCount += doc["tracks"].GetArray().Size();
                }
            }
        }

        BSML::MainThreadScheduler::Schedule([this, sizeInBytes, entryCount]() {
            friendHistoryCacheSizeTextView_->set_text(std::format("({} entries, {})", entryCount, getHumanReadableSize(sizeInBytes)));

            const bool clearing = isClearingFriendHistory_.load();
            clearFriendHistoryButton_->set_interactable(!clearing);
        });
    }).detach();
}

void SettingsViewController::onClearHistoryButtonClicked() {
    const std::string selectedRange = historyClearRangeValue_;
    if (selectedRange == "Older than 1 day") {
        clearHistoryOlderThan(std::chrono::hours(1 * 24));
    } else if (selectedRange == "Older than 1 week") {
        clearHistoryOlderThan(std::chrono::hours(7 * 24));
    } else if (selectedRange == "Older than 1 month") {
        clearHistoryOlderThan(std::chrono::hours(30 * 24));
    } else if (selectedRange == "All entries") {
        clearAllHistory();
    }
}

void SettingsViewController::onClearFriendHistoryButtonClicked() {
    const std::string selectedRange = friendHistoryClearRangeValue_;
    if (selectedRange == "Older than 1 day") {
        clearFriendHistoryOlderThan(std::chrono::hours(1 * 24));
    } else if (selectedRange == "Older than 1 week") {
        clearFriendHistoryOlderThan(std::chrono::hours(7 * 24));
    } else if (selectedRange == "Older than 1 month") {
        clearFriendHistoryOlderThan(std::chrono::hours(30 * 24));
    } else if (selectedRange == "All entries") {
        clearAllFriendHistory();
    }
}

void SettingsViewController::clearHistoryOlderThan(std::chrono::hours age) {
    if (isClearingHistory_) {
        return;
    }
    isClearingHistory_ = true;
    historyCacheSizeTextView_->set_text("(Clearing...)");
    clearHistoryButton_->set_interactable(false);

    std::thread([this, age]() {
        const std::filesystem::path cachePath = AirbudsSearch::getDataDirectory() / "recently_played_cache.json";

        if (!std::filesystem::exists(cachePath)) {
            isClearingHistory_ = false;
            BSML::MainThreadScheduler::Schedule([this]() {
                refreshHistoryCacheSizeStatus();
            });
            return;
        }

        std::ifstream inFile(cachePath, std::ios::binary);
        if (!inFile.is_open()) {
            isClearingHistory_ = false;
            BSML::MainThreadScheduler::Schedule([this]() {
                refreshHistoryCacheSizeStatus();
            });
            return;
        }

        std::string data((std::istreambuf_iterator<char>(inFile)), std::istreambuf_iterator<char>());
        inFile.close();

        rapidjson::Document doc;
        doc.Parse(data.c_str());

        if (!doc.IsObject() || !doc.HasMember("tracks") || !doc["tracks"].IsArray()) {
            isClearingHistory_ = false;
            BSML::MainThreadScheduler::Schedule([this]() {
                refreshHistoryCacheSizeStatus();
            });
            return;
        }

        const auto now = std::chrono::system_clock::now();
        const auto cutoffTime = std::chrono::duration_cast<std::chrono::milliseconds>(
            (now - age).time_since_epoch()
        );

        rapidjson::Value newTracks(rapidjson::kArrayType);
        auto& allocator = doc.GetAllocator();

        for (const auto& track : doc["tracks"].GetArray()) {
            if (!track.IsObject()) {
                continue;
            }
            int64_t playedAtMs = 0;
            if (track.HasMember("playedAtMs") && track["playedAtMs"].IsInt64()) {
                playedAtMs = track["playedAtMs"].GetInt64();
            }
            if (playedAtMs >= cutoffTime.count()) {
                rapidjson::Value trackCopy;
                trackCopy.CopyFrom(track, allocator);
                newTracks.PushBack(trackCopy, allocator);
            }
        }

        doc["tracks"] = newTracks;

        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        doc.Accept(writer);

        std::ofstream outFile(cachePath, std::ios::binary | std::ios::trunc);
        if (outFile.is_open()) {
            outFile.write(buffer.GetString(), buffer.GetSize());
        }

        isClearingHistory_ = false;
        BSML::MainThreadScheduler::Schedule([this]() {
            refreshHistoryCacheSizeStatus();
        });
    }).detach();
}

void SettingsViewController::clearAllHistory() {
    if (isClearingHistory_) {
        return;
    }
    isClearingHistory_ = true;
    historyCacheSizeTextView_->set_text("(Clearing...)");
    clearHistoryButton_->set_interactable(false);

    std::thread([this]() {
        const std::filesystem::path cachePath = AirbudsSearch::getDataDirectory() / "recently_played_cache.json";
        if (std::filesystem::exists(cachePath)) {
            std::filesystem::remove(cachePath);
        }
        isClearingHistory_ = false;
        BSML::MainThreadScheduler::Schedule([this]() {
            refreshHistoryCacheSizeStatus();
        });
    }).detach();
}

void SettingsViewController::clearFriendHistoryOlderThan(std::chrono::hours age) {
    if (isClearingFriendHistory_) {
        return;
    }
    isClearingFriendHistory_ = true;
    friendHistoryCacheSizeTextView_->set_text("(Clearing...)");
    clearFriendHistoryButton_->set_interactable(false);

    std::thread([this, age]() {
        const std::filesystem::path cacheDir = AirbudsSearch::getDataDirectory() / std::string(FRIEND_HISTORY_CACHE_DIR);

        if (!std::filesystem::exists(cacheDir)) {
            isClearingFriendHistory_ = false;
            BSML::MainThreadScheduler::Schedule([this]() {
                refreshFriendHistoryCacheSizeStatus();
            });
            return;
        }

        const auto now = std::chrono::system_clock::now();
        const auto cutoffTime = std::chrono::duration_cast<std::chrono::milliseconds>(
            (now - age).time_since_epoch()
        );

        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(cacheDir)) {
            if (!entry.is_regular_file()) {
                continue;
            }

            std::ifstream inFile(entry.path(), std::ios::binary);
            if (!inFile.is_open()) {
                continue;
            }

            std::string data((std::istreambuf_iterator<char>(inFile)), std::istreambuf_iterator<char>());
            inFile.close();

            rapidjson::Document doc;
            doc.Parse(data.c_str());

            if (!doc.IsObject() || !doc.HasMember("tracks") || !doc["tracks"].IsArray()) {
                continue;
            }

            rapidjson::Value newTracks(rapidjson::kArrayType);
            auto& allocator = doc.GetAllocator();

            for (const auto& track : doc["tracks"].GetArray()) {
                if (!track.IsObject()) {
                    continue;
                }
                int64_t playedAtMs = 0;
                if (track.HasMember("playedAtMs") && track["playedAtMs"].IsInt64()) {
                    playedAtMs = track["playedAtMs"].GetInt64();
                }
                if (playedAtMs >= cutoffTime.count()) {
                    rapidjson::Value trackCopy;
                    trackCopy.CopyFrom(track, allocator);
                    newTracks.PushBack(trackCopy, allocator);
                }
            }

            if (newTracks.Empty()) {
                std::filesystem::remove(entry.path());
                continue;
            }

            doc["tracks"] = newTracks;

            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            doc.Accept(writer);

            std::ofstream outFile(entry.path(), std::ios::binary | std::ios::trunc);
            if (outFile.is_open()) {
                outFile.write(buffer.GetString(), buffer.GetSize());
            }
        }

        isClearingFriendHistory_ = false;
        BSML::MainThreadScheduler::Schedule([this]() {
            refreshFriendHistoryCacheSizeStatus();
        });
    }).detach();
}

void SettingsViewController::clearAllFriendHistory() {
    if (isClearingFriendHistory_) {
        return;
    }
    isClearingFriendHistory_ = true;
    friendHistoryCacheSizeTextView_->set_text("(Clearing...)");
    clearFriendHistoryButton_->set_interactable(false);

    std::thread([this]() {
        const std::filesystem::path cacheDir = AirbudsSearch::getDataDirectory() / std::string(FRIEND_HISTORY_CACHE_DIR);
        if (std::filesystem::exists(cacheDir)) {
            std::filesystem::remove_all(cacheDir);
        }
        isClearingFriendHistory_ = false;
        BSML::MainThreadScheduler::Schedule([this]() {
            refreshFriendHistoryCacheSizeStatus();
        });
    }).detach();
}
