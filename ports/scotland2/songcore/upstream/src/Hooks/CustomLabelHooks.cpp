#include "hooking.hpp"
#include "logging.hpp"

#include "UI/IconCache.hpp"
#include "GlobalNamespace/BeatmapCharacteristicSO.hpp"
#include "GlobalNamespace/BeatmapCharacteristicCollection.hpp"
#include "GlobalNamespace/BeatmapCharacteristicSegmentedControlController.hpp"
#include "GlobalNamespace/BeatmapDifficultySegmentedControlController.hpp"
#include "GlobalNamespace/BeatLineManager.hpp"
#include "GlobalNamespace/StandardLevelDetailView.hpp"
#include "GlobalNamespace/BeatmapDifficultyMethods.hpp"
#include "HMUI/IconSegmentedControl.hpp"
#include "HMUI/TextSegmentedControl.hpp"
#include "System/Collections/Generic/IReadOnlyList_1.hpp"
#include "System/Collections/Generic/IReadOnlyCollection_1.hpp"
#include "System/Collections/Generic/IEnumerable_1.hpp"
#include "System/Collections/Generic/IEnumerator_1.hpp"
#include "System/Collections/IEnumerable.hpp"
#include "System/Collections/IEnumerator.hpp"
#include "System/IDisposable.hpp"

#include "SongLoader/CustomBeatmapLevel.hpp"
#include "CustomJSONData.hpp"

struct TemporaryCharacteristicSegmentedControlData {
    int sortingOrder;
    UnityEngine::Sprite* icon;
    std::string label;
};

/// @brief method to set the custom characteristic labels on the passed segmented controller
void SetCustomCharacteristicLabels(GlobalNamespace::BeatmapCharacteristicSegmentedControlController* self, SongCore::SongLoader::CustomBeatmapLevel* customLevel);
/// @brief method to set the custom difficulty labels on the passed segmented controller
void SetCustomDifficultyLabels(GlobalNamespace::BeatmapDifficultySegmentedControlController* self, SongCore::SongLoader::CustomBeatmapLevel* customLevel, GlobalNamespace::BeatmapKey const& beatmapKey);

MAKE_AUTO_HOOK_MATCH(
    StandardLevelDetailView_SetContent,
    static_cast<
    void(GlobalNamespace::StandardLevelDetailView::*)(
        ::GlobalNamespace::BeatmapLevel* level,
        ::GlobalNamespace::BeatmapDifficultyMask allowedBeatmapDifficultyMask,
        ::System::Collections::Generic::HashSet_1<::UnityW<::GlobalNamespace::BeatmapCharacteristicSO>>* notAllowedCharacteristics,
        ::GlobalNamespace::BeatmapDifficulty defaultDifficulty,
        ::GlobalNamespace::BeatmapCharacteristicSO* defaultBeatmapCharacteristic,
        ::GlobalNamespace::PlayerData* playerData
    )>(&GlobalNamespace::StandardLevelDetailView::SetContent),
    void,
    GlobalNamespace::StandardLevelDetailView* self,
    ::GlobalNamespace::BeatmapLevel* level,
    ::GlobalNamespace::BeatmapDifficultyMask allowedBeatmapDifficultyMask,
    ::System::Collections::Generic::HashSet_1<::UnityW<::GlobalNamespace::BeatmapCharacteristicSO>>* notAllowedCharacteristics,
    ::GlobalNamespace::BeatmapDifficulty defaultDifficulty,
    ::GlobalNamespace::BeatmapCharacteristicSO* defaultBeatmapCharacteristic,
    ::GlobalNamespace::PlayerData* playerData
) {
    StandardLevelDetailView_SetContent(
        self,
        level,
        allowedBeatmapDifficultyMask,
        notAllowedCharacteristics,
        defaultDifficulty,
        defaultBeatmapCharacteristic,
        playerData
    );

    auto customLevel = il2cpp_utils::try_cast<SongCore::SongLoader::CustomBeatmapLevel>(level).value_or(nullptr);
    if (!customLevel) return;

    SetCustomCharacteristicLabels(self->_beatmapCharacteristicSegmentedControlController, customLevel);
    SetCustomDifficultyLabels(self->_beatmapDifficultySegmentedControlController, customLevel, self->beatmapKey);
}

// after characteristic is selected, we need to update the diff labels again
MAKE_AUTO_HOOK_MATCH(
    StandardLevelDetailView_HandleBeatmapCharacteristicSegmentedControlControllerDidSelectBeatmapCharacteristic,
    &GlobalNamespace::StandardLevelDetailView::HandleBeatmapCharacteristicSegmentedControlControllerDidSelectBeatmapCharacteristic,
    void,
    GlobalNamespace::StandardLevelDetailView* self,
    GlobalNamespace::BeatmapCharacteristicSegmentedControlController* controller,
    GlobalNamespace::BeatmapCharacteristicSO* beatmapCharacteristic
) {
    StandardLevelDetailView_HandleBeatmapCharacteristicSegmentedControlControllerDidSelectBeatmapCharacteristic(
        self,
        controller,
        beatmapCharacteristic
    );

    auto customLevel = il2cpp_utils::try_cast<SongCore::SongLoader::CustomBeatmapLevel>(self->_beatmapLevel).value_or(nullptr);
    if (!customLevel) return;

    SetCustomDifficultyLabels(self->_beatmapDifficultySegmentedControlController, customLevel, self->beatmapKey);
}

void SetCustomCharacteristicLabels(GlobalNamespace::BeatmapCharacteristicSegmentedControlController* self, SongCore::SongLoader::CustomBeatmapLevel* customLevel) {
    auto customSaveDataInfoOpt = customLevel->CustomSaveDataInfo;
    if (!customSaveDataInfoOpt) return;
    auto& customSaveDataInfo = customSaveDataInfoOpt->get();

    auto beatmapCharacteristics = ListW<UnityW<GlobalNamespace::BeatmapCharacteristicSO>>(self->_currentlyAvailableBeatmapCharacteristics);
    std::vector<TemporaryCharacteristicSegmentedControlData> cellData;
    auto success = !beatmapCharacteristics.empty();
    int selectedCellIdx = self->_segmentedControl->selectedCellNumber;

    for (auto characteristic : beatmapCharacteristics) {
        auto characteristicDetailsOpt = customSaveDataInfo.TryGetCharacteristic(characteristic->serializedName);
        if (!characteristicDetailsOpt.has_value()) { success = false; break; }
        auto& characteristicDetails = characteristicDetailsOpt->get();

        auto label = characteristicDetails.characteristicLabel.value_or(characteristicDetails.characteristicName);
        UnityEngine::Sprite* icon = nullptr;
        if (characteristicDetails.characteristicIconImageFileName.has_value() && !characteristicDetails.characteristicIconImageFileName->empty()) {
            auto iconCache = SongCore::UI::IconCache::get_instance();
            if (iconCache) { // if iconcache instance exists, use it
                auto iconPath = std::filesystem::path(customLevel->get_customLevelPath()) / characteristicDetails.characteristicIconImageFileName.value();
                icon = iconCache->GetIconForPath(iconPath);
            }
        }

        cellData.emplace_back(
            characteristic->sortingOrder,
            icon ? icon : characteristic->icon.unsafePtr(),
            label
        );
    }

    if (success) {
        ArrayW<HMUI::IconSegmentedControl::DataItem*> dataItems(cellData.size());
        for (int i = 0; auto& [_, icon, label] : cellData) {
            dataItems[i++] = HMUI::IconSegmentedControl::DataItem::New_ctor(
                icon, label, true
            );
        }

        self->_segmentedControl->SetData(dataItems);
        self->_segmentedControl->SelectCellWithNumber(selectedCellIdx);
    }
}

void SetCustomDifficultyLabels(GlobalNamespace::BeatmapDifficultySegmentedControlController* self, SongCore::SongLoader::CustomBeatmapLevel* customLevel, GlobalNamespace::BeatmapKey const& beatmapKey) {
    auto customSaveDataInfoOpt = customLevel->CustomSaveDataInfo;
    if (!customSaveDataInfoOpt) return;
    auto& customSaveDataInfo = customSaveDataInfoOpt->get();

    auto characteristic = beatmapKey.beatmapCharacteristic;
    auto difficulties = ListW<GlobalNamespace::BeatmapDifficulty>(self->_difficulties);
    auto labels = ListW<StringW>::New();

    auto success = !difficulties.empty();
    int selectedCellIdx = self->_difficultySegmentedControl->selectedCellNumber;

    for (auto difficulty : difficulties) {
        auto difficultyDetailsOpt = customSaveDataInfo.TryGetCharacteristicAndDifficulty(characteristic->serializedName, difficulty);
        if (!difficultyDetailsOpt.has_value()) { success = false; break; }

        auto& difficultyDetails = difficultyDetailsOpt->get();
        if (difficultyDetails.customDiffLabel.has_value() && !difficultyDetails.customDiffLabel->empty()) {
            labels->Add(difficultyDetails.customDiffLabel.value());
        } else {
            labels->Add(GlobalNamespace::BeatmapDifficultyMethods::Name(difficulty));
        }
    }

    if (success) { // we found labels
        self->_difficultySegmentedControl->SetTexts(labels->i___System__Collections__Generic__IReadOnlyList_1_T_(), nullptr);
        self->_difficultySegmentedControl->SelectCellWithNumber(selectedCellIdx);
    }
}
