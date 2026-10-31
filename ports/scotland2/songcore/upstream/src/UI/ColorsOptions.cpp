#include "UI/ColorsOptions.hpp"
#include "config.hpp"
#include "assets.hpp"
#include "logging.hpp"

#include "UnityEngine/Resources.hpp"
#include "bsml/shared/BSML.hpp"
#include "bsml/shared/Helpers/delegates.hpp"

DEFINE_TYPE(SongCore::UI, ColorsOptions);

namespace SongCore::UI {
    void ColorsOptions::ctor(GlobalNamespace::StandardLevelDetailViewController* levelDetailViewController) {
        INVOKE_CTOR();
        _levelDetailViewController = levelDetailViewController;
        _voidColor = {0.5f, 0.5f, 0.5f, 0.25f};
        _modalHideAction = BSML::MakeSystemAction<>(
            [this](){
                if(onModalHide) onModalHide();
            }
        );
    }

    void ColorsOptions::Show() {
        _colorsOptionsModal->Show();
    }

    void ColorsOptions::ShowColors(CustomJSONData::CustomSaveDataInfo::BasicCustomDifficultyBeatmapDetails const& details) {
        Parse();
        _colorsOptionsModal->transform->position = _modalPosition;
        Show();
        SetColors(details.customColors.value());
    }

    void ColorsOptions::Parse() {
        if (!_colorsOptionsModal) {
            BSML::parse_and_construct(Assets::colors_bsml, _levelDetailViewController->_standardLevelDetailView->transform, this);
            _modalPosition = _colorsOptionsModal->transform->position;
        }
    }

    void ColorsOptions::PostParse() {
        auto colorSchemeViewPrefab = UnityEngine::Resources::FindObjectsOfTypeAll<GlobalNamespace::ColorSchemeView*>()->First();
        auto colorSchemeViewObject = UnityEngine::Object::Instantiate(colorSchemeViewPrefab->gameObject, _selectedColorBG);
        _colorSchemeView = colorSchemeViewObject->GetComponent<GlobalNamespace::ColorSchemeView*>();
        _colorsOptionsModal->onHide = std::bind(&ColorsOptions::Dismiss, this);
    }

    void ColorsOptions::Dismiss() {
        _colorsOptionsModal->HMUI::ModalView::Hide(false, _modalHideAction);
    }

    bool ColorsOptions::get_customSongNoteColors() { return config.customSongNoteColors; }
    void ColorsOptions::set_customSongNoteColors(bool value) {
        config.customSongNoteColors = value;
        SaveConfig();
    }

    bool ColorsOptions::get_customSongObstacleColors() { return config.customSongObstacleColors; }
    void ColorsOptions::set_customSongObstacleColors(bool value) {
        config.customSongObstacleColors = value;
        SaveConfig();
    }

    bool ColorsOptions::get_customSongEnvironmentColors() { return config.customSongEnvironmentColors; }
    void ColorsOptions::set_customSongEnvironmentColors(bool value) {
        config.customSongEnvironmentColors = value;
        SaveConfig();
    }

    void ColorsOptions::SetColors(CustomJSONData::CustomSaveDataInfo::BasicCustomDifficultyBeatmapDetails::CustomColors const& colors) {
        _colorSchemeView->SetColors(
            colors.colorLeft.value_or(_voidColor),
            colors.colorRight.value_or(_voidColor),
            colors.envColorLeft.value_or(_voidColor),
            colors.envColorRight.value_or(_voidColor),
            colors.envColorLeftBoost.value_or(_voidColor),
            colors.envColorRightBoost.value_or(_voidColor),
            colors.obstacleColor.value_or(_voidColor)
        );
    }
}
