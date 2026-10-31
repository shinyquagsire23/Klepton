#include "hooking.hpp"
#include "logging.hpp"

#include "GlobalNamespace/BeatmapCharacteristicCollection.hpp"
#include "SongCore.hpp"

// characteristic used if none is found
#define MISSING_CHARACTERISTIC "MissingCharacteristic"

MAKE_AUTO_HOOK_MATCH(BeatmapCharacteristicCollection_GetBeatmapCharacteristicBySerializedName, &GlobalNamespace::BeatmapCharacteristicCollection::GetBeatmapCharacteristicBySerializedName, UnityW<GlobalNamespace::BeatmapCharacteristicSO>, GlobalNamespace::BeatmapCharacteristicCollection* self, StringW serializedName) {
    auto result = BeatmapCharacteristicCollection_GetBeatmapCharacteristicBySerializedName(self, serializedName);
    if (!result) {
        std::string cppSerializedName(serializedName);
        INFO("GetBeatmapCharacteristicBySerializedName failed to find characteristic with serialized name '{}'", cppSerializedName);
        result = SongCore::API::Characteristics::GetCharacteristicBySerializedName(cppSerializedName);
        if (!result) {
            WARNING("GetBeatmapCharacteristicBySerializedName STILL failed to find characteristic with serialized name '{}', returning '{}' instead!", cppSerializedName, MISSING_CHARACTERISTIC);
            result = SongCore::API::Characteristics::GetCharacteristicBySerializedName(MISSING_CHARACTERISTIC);
        }
    }

    return result;
}
