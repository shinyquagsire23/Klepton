#pragma once

#include "custom-types/shared/macros.hpp"
#include "System/Object.hpp"
#include "CustomJSONData.hpp"
#include "GlobalNamespace/StandardLevelDetailViewController.hpp"
#include "GlobalNamespace/ColorSchemeView.hpp"
#include "bsml/shared/BSML/Components/Settings/ToggleSetting.hpp"
#include "bsml/shared/BSML/Components/ModalView.hpp"

DECLARE_CLASS_CODEGEN(SongCore::UI, ColorsOptions, System::Object) {
        DECLARE_CTOR(ctor, GlobalNamespace::StandardLevelDetailViewController* levelDetailViewController);

        DECLARE_INSTANCE_FIELD_PRIVATE(GlobalNamespace::StandardLevelDetailViewController*, _levelDetailViewController);
        DECLARE_INSTANCE_FIELD_PRIVATE(GlobalNamespace::ColorSchemeView*, _colorSchemeView);
        DECLARE_INSTANCE_FIELD_PRIVATE(BSML::ModalView*, _colorsOptionsModal);
        DECLARE_INSTANCE_FIELD_PRIVATE(UnityEngine::Vector3, _modalPosition);
        DECLARE_INSTANCE_FIELD_PRIVATE(UnityEngine::RectTransform*, _selectedColorBG);
        DECLARE_INSTANCE_FIELD_PRIVATE(UnityEngine::Color, _voidColor);
        DECLARE_INSTANCE_FIELD_PRIVATE(System::Action*, _modalHideAction);

        DECLARE_INSTANCE_METHOD(void, PostParse);

        DECLARE_INSTANCE_METHOD(bool, get_customSongNoteColors);
        DECLARE_INSTANCE_METHOD(void, set_customSongNoteColors, bool value);
        DECLARE_INSTANCE_METHOD(bool, get_customSongObstacleColors);
        DECLARE_INSTANCE_METHOD(void, set_customSongObstacleColors, bool value);
        DECLARE_INSTANCE_METHOD(bool, get_customSongEnvironmentColors);
        DECLARE_INSTANCE_METHOD(void, set_customSongEnvironmentColors, bool value);
        DECLARE_INSTANCE_METHOD(void, Dismiss);
    public:
        void Show();
        void Parse();
        void SetColors(CustomJSONData::CustomSaveDataInfo::BasicCustomDifficultyBeatmapDetails::CustomColors const& colors);

        void ShowColors(CustomJSONData::CustomSaveDataInfo::BasicCustomDifficultyBeatmapDetails const& details);
        std::function<void()> onModalHide;
};