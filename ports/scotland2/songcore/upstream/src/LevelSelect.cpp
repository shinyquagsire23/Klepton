#include "LevelSelect.hpp"
#include "logging.hpp"

#include "CustomJSONData.hpp"
#include "SongCore.hpp"

#include "SongLoader/RuntimeSongLoader.hpp"

#include "SongLoader/RuntimeSongLoader.hpp"
#include "bsml/shared/Helpers/delegates.hpp"

DEFINE_TYPE(SongCore, LevelSelect);

namespace SongCore {
    void LevelSelect::ctor(GlobalNamespace::StandardLevelDetailViewController* levelDetailViewController) {
        INVOKE_CTOR();
        _levelDetailViewController = levelDetailViewController;
    }

    void LevelSelect::Initialize() {
        _changeDifficultyBeatmapAction = BSML::MakeSystemAction<UnityW<GlobalNamespace::StandardLevelDetailViewController>>(
            std::function<void(UnityW<GlobalNamespace::StandardLevelDetailViewController>)>(
                std::bind(&LevelSelect::BeatmapLevelSelected, this, std::placeholders::_1)
            )
        );

        _levelDetailViewController->add_didChangeDifficultyBeatmapEvent(_changeDifficultyBeatmapAction);

        _changeContentAction = custom_types::MakeDelegate<System::Action_2<UnityW<GlobalNamespace::StandardLevelDetailViewController>, GlobalNamespace::StandardLevelDetailViewController::ContentType>*>(
            std::function<void(UnityW<GlobalNamespace::StandardLevelDetailViewController>, GlobalNamespace::StandardLevelDetailViewController::ContentType)>(
                std::bind(&LevelSelect::LevelDetailContentChanged, this, std::placeholders::_1, std::placeholders::_2)
            )
        );

        _levelDetailViewController->add_didChangeContentEvent(_changeContentAction);
    }

    void LevelSelect::Dispose() {
        _levelDetailViewController->remove_didChangeDifficultyBeatmapEvent(_changeDifficultyBeatmapAction);
        _levelDetailViewController->remove_didChangeContentEvent(_changeContentAction);
    }

    void LevelSelect::LevelDetailContentChanged(GlobalNamespace::StandardLevelDetailViewController* levelDetailViewController, GlobalNamespace::StandardLevelDetailViewController::ContentType contentType) {
        if (contentType == GlobalNamespace::StandardLevelDetailViewController::ContentType::OwnedAndReady) {
            StartLevelWasSelectedInvoke();
        }
    }

    void LevelSelect::BeatmapLevelSelected(GlobalNamespace::StandardLevelDetailViewController* levelDetailViewController) {
        StartLevelWasSelectedInvoke();
    }

    void LevelSelect::StartLevelWasSelectedInvoke() {
        LevelWasSelectedEventArgs eventArgs;

        eventArgs.levelPack = GetSelectedLevelPack();
        eventArgs.beatmapLevel = GetSelectedBeatmapLevel();
        eventArgs.beatmapKey = GetSelectedBeatmapKey();
        eventArgs.isCustom = false;

        // if no beatmap level selected, return
        if (!eventArgs.beatmapKey.beatmapCharacteristic) return;
        if (!eventArgs.beatmapLevel) return;

        eventArgs.levelID = static_cast<std::string>(eventArgs.beatmapKey.levelId);

        auto customLevel = il2cpp_utils::try_cast<SongLoader::CustomBeatmapLevel>(eventArgs.beatmapLevel).value_or(nullptr);
        if (customLevel) {
            eventArgs.isCustom = true;
            HandleCustomLevelWasSelected(eventArgs);
        }

        InvokeLevelWasSelected(eventArgs);
    }

    void LevelSelect::HandleCustomLevelWasSelected(LevelWasSelectedEventArgs& eventArgs) {
        if (!eventArgs.isCustom) return;

        eventArgs.isWIP = eventArgs.levelID.ends_with(" WIP");
        // get a view into the levelid
        std::string_view hashView(eventArgs.levelID);
        // remove `custom_level_` from the front
        hashView = hashView.substr(SongLoader::RuntimeSongLoader::CUSTOM_LEVEL_PREFIX_ID.size());
        // possibly remove ` WIP` from the end
        eventArgs.hash = hashView.substr(0, hashView.size() - (eventArgs.isWIP ? 4 : 0));

        auto customLevel = eventArgs.customBeatmapLevel;
        if (customLevel->standardLevelInfoSaveDataV2) {
            eventArgs.customLevelInfoSaveDataV2 = customLevel->standardLevelInfoSaveDataV2;
        } else if (customLevel->beatmapLevelSaveDataV4) {
            eventArgs.customBeatmapLevelSaveDataV4 = customLevel->beatmapLevelSaveDataV4;
        } else return;

        auto saveData = customLevel->CustomSaveDataInfo;
        if (!saveData.has_value()) return;

        auto levelDetails = saveData->get().TryGetBasicLevelDetails();
        if (!levelDetails.has_value()) return;

        auto characteristicDetails = levelDetails->get().TryGetCharacteristic(eventArgs.beatmapKey.beatmapCharacteristic->serializedName);
        if (!characteristicDetails.has_value()) return;

        auto diffDetails = characteristicDetails->get().TryGetDifficulty(eventArgs.beatmapKey.difficulty);
        if (!diffDetails.has_value()) return;

        eventArgs.customLevelDetails.emplace(
            levelDetails.value(),
            characteristicDetails.value(),
            diffDetails.value()
        );
    }

    void LevelSelect::InvokeLevelWasSelected(LevelWasSelectedEventArgs const& eventArgs) {
        LevelWasSelected.invoke(eventArgs);
        SongCore::API::LevelSelect::GetLevelWasSelectedEvent().invoke(eventArgs);
    }

    GlobalNamespace::BeatmapLevelPack* LevelSelect::GetSelectedLevelPack() {
        return _levelDetailViewController->_pack;
    }

    GlobalNamespace::BeatmapLevel* LevelSelect::GetSelectedBeatmapLevel() {
        return _levelDetailViewController->beatmapLevel;
    }

    GlobalNamespace::BeatmapKey LevelSelect::GetSelectedBeatmapKey() {
        return _levelDetailViewController->beatmapKey;
    }

    GlobalNamespace::BeatmapCharacteristicSO* LevelSelect::GetSelectedCharacteristic() {
        return _levelDetailViewController->beatmapKey.beatmapCharacteristic;
    }

    GlobalNamespace::BeatmapDifficulty LevelSelect::GetSelectedDifficulty() {
        return _levelDetailViewController->beatmapKey.difficulty;
    }
}
