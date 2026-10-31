#include "hooking.hpp"
#include "logging.hpp"

#include "GlobalNamespace/BeatmapDataTransformHelper.hpp"
#include "GlobalNamespace/BeatmapDataObstaclesMergingTransform.hpp"

#include "GlobalNamespace/BoolSO.hpp"
#include "GlobalNamespace/BeatmapLevel.hpp"

// hooks to get around the `IsObstaclesMergingNeeded` if statement being different on quest.
// if it's a custom level we want to block merging, we do this by hooking the method that performs the merge and just not performing it
bool ByPassMergeTransformForCustomLevel = false;
MAKE_AUTO_HOOK_MATCH(
    BeatmapDataTransformHelper_CreateTransformedBeatmapData,
    &GlobalNamespace::BeatmapDataTransformHelper::CreateTransformedBeatmapData,
    GlobalNamespace::IReadonlyBeatmapData*,
    GlobalNamespace::IReadonlyBeatmapData* beatmapData,
    GlobalNamespace::BeatmapLevel* beatmapLevel,
    GlobalNamespace::GameplayModifiers* gameplayModifiers,
    bool leftHanded,
    GlobalNamespace::EnvironmentEffectsFilterPreset environmentEffectsFilterPreset,
    GlobalNamespace::EnvironmentIntensityReductionOptions* environmentIntensityReductionOptions,
    ByRef<::BeatSaber::Settings::Settings> settings
) {
    ByPassMergeTransformForCustomLevel = beatmapLevel && beatmapLevel->levelID.starts_with(u"custom_level_");

    return BeatmapDataTransformHelper_CreateTransformedBeatmapData(
        beatmapData,
        beatmapLevel,
        gameplayModifiers,
        leftHanded,
        environmentEffectsFilterPreset,
        environmentIntensityReductionOptions,
        settings
    );
}

MAKE_AUTO_HOOK_MATCH(
    BeatmapDataObstaclesMergingTransform_CreateTransformedData,
    &GlobalNamespace::BeatmapDataObstaclesMergingTransform::CreateTransformedData,
    GlobalNamespace::IReadonlyBeatmapData*,
    GlobalNamespace::IReadonlyBeatmapData* data
) {
    if (ByPassMergeTransformForCustomLevel) return data;
    else return BeatmapDataObstaclesMergingTransform_CreateTransformedData(data);
}
