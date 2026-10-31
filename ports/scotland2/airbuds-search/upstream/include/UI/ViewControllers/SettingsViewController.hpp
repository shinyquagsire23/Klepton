#pragma once

#include <custom-types/shared/macros.hpp>
#include <bsml/shared/BSML.hpp>
#include <TMPro/TextMeshProUGUI.hpp>
#include <HMUI/InputFieldView.hpp>

#include "Log.hpp"
#include "Modal.hpp"
#include "UI/ModalView.hpp"

#if HOT_RELOAD
#include <bsml/shared/BSML/ViewControllers/HotReloadViewController.hpp>
using BaseViewController = BSML::HotReloadViewController;
#else
#include <HMUI/ViewController.hpp>
using BaseViewController = HMUI::ViewController;
#endif

DECLARE_CLASS_CODEGEN_INTERFACES(AirbudsSearch::UI::ViewControllers, SettingsViewController, BaseViewController) {

    DECLARE_OVERRIDE_METHOD_MATCH(void, DidActivate, &HMUI::ViewController::DidActivate, bool isFirstActivation, bool addedToHierarchy, bool screenSystemDisabling);

    DECLARE_INSTANCE_METHOD(void, PostParse);

    // Airbuds refresh token
    DECLARE_INSTANCE_FIELD(UnityW<HMUI::InputFieldView>, refreshTokenTextField_);
    DECLARE_INSTANCE_FIELD(UnityW<UnityEngine::UI::Button>, refreshTokenPasteButton_);
    DECLARE_INSTANCE_FIELD(UnityW<UnityEngine::UI::Button>, refreshTokenClearButton_);
    DECLARE_INSTANCE_FIELD(UnityW<UnityEngine::UI::Button>, refreshTokenSaveButton_);
    DECLARE_INSTANCE_FIELD(UnityW<TMPro::TextMeshProUGUI>, refreshTokenStatusTextView_);
    DECLARE_INSTANCE_METHOD(void, onPasteRefreshTokenButtonClicked);
    DECLARE_INSTANCE_METHOD(void, onClearRefreshTokenButtonClicked);
    DECLARE_INSTANCE_METHOD(void, onSaveRefreshTokenButtonClicked);

    // Kakasi Mod Status
    DECLARE_INSTANCE_FIELD(UnityW<TMPro::TextMeshProUGUI>, kakasiStatusTextView_);

    // Image Cache
    DECLARE_INSTANCE_FIELD(UnityW<TMPro::TextMeshProUGUI>, cacheSizeTextView_);
    DECLARE_INSTANCE_FIELD(UnityW<UnityEngine::UI::Button>, clearCacheButton_);
    DECLARE_INSTANCE_METHOD(void, onClearCacheButtonClicked);

    // History Cache
    DECLARE_INSTANCE_FIELD(UnityW<TMPro::TextMeshProUGUI>, historyCacheSizeTextView_);
    DECLARE_INSTANCE_FIELD(ListW<StringW>, historyClearRangeOptions_);
    DECLARE_INSTANCE_FIELD(StringW, historyClearRangeValue_);
    DECLARE_INSTANCE_FIELD(UnityW<UnityEngine::UI::Button>, clearHistoryButton_);
    DECLARE_INSTANCE_METHOD(void, onClearHistoryButtonClicked);

    // Friend History Cache
    DECLARE_INSTANCE_FIELD(UnityW<TMPro::TextMeshProUGUI>, friendHistoryCacheSizeTextView_);
    DECLARE_INSTANCE_FIELD(ListW<StringW>, friendHistoryClearRangeOptions_);
    DECLARE_INSTANCE_FIELD(StringW, friendHistoryClearRangeValue_);
    DECLARE_INSTANCE_FIELD(UnityW<UnityEngine::UI::Button>, clearFriendHistoryButton_);
    DECLARE_INSTANCE_METHOD(void, onClearFriendHistoryButtonClicked);

    // Modal
    DECLARE_INSTANCE_FIELD(UnityW<ModalView>, modalView_);

    private:
    void refreshAirbudsTokenStatus();
    void refreshKakasiStatus();
    void refreshCacheSizeStatus();
    void refreshHistoryCacheSizeStatus();
    void refreshFriendHistoryCacheSizeStatus();

    void refresh();

    void clearHistoryOlderThan(std::chrono::hours age);
    void clearAllHistory();
    void clearFriendHistoryOlderThan(std::chrono::hours age);
    void clearAllFriendHistory();

    uintmax_t getDirectorySizeInBytes(const std::filesystem::path& path);
    std::string getHumanReadableSize(uintmax_t bytes);

    std::atomic_bool isClearingCache_;
    std::atomic_bool isClearingHistory_;
    std::atomic_bool isClearingFriendHistory_;
};
