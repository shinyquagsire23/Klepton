#include "Characteristics.hpp"
#include "SongCore.hpp"

#include "System/Collections/Generic/Dictionary_2.hpp"
#include <mutex>

DEFINE_TYPE(SongCore, Characteristics);

static ListW<UnityW<GlobalNamespace::BeatmapCharacteristicSO>> ToList(System::Collections::Generic::Dictionary_2<StringW, UnityW<GlobalNamespace::BeatmapCharacteristicSO>>::ValueCollection* col) {
    return {};
}

namespace SongCore {
    void Characteristics::ctor(GlobalNamespace::BeatmapCharacteristicCollection* beatmapCharacteristicCollection, GlobalNamespace::AppStaticSettingsSO* appStaticSettings) {
        _beatmapCharacteristicCollection = beatmapCharacteristicCollection;
        _appStaticSettings = appStaticSettings;

        auto characteristics = il2cpp_utils::cast<System::Collections::Generic::List_1<UnityW<GlobalNamespace::BeatmapCharacteristicSO>>>(_beatmapCharacteristicCollection->beatmapCharacteristics);
        _beatmapCharacteristics = ListW<UnityW<GlobalNamespace::BeatmapCharacteristicSO>>(characteristics);

        auto disabledCharacteristics = il2cpp_utils::cast<System::Collections::Generic::List_1<UnityW<GlobalNamespace::BeatmapCharacteristicSO>>>(_beatmapCharacteristicCollection->disabledBeatmapCharacteristics);
        _disabledBeatmapCharacteristics = ListW<UnityW<GlobalNamespace::BeatmapCharacteristicSO>>(disabledCharacteristics);
    }

    void Characteristics::Initialize() {
        SongCore::API::Characteristics::GetCharacteristicsUpdatedEvent() += {&Characteristics::CharacteristicsUpdated, this};
        // on initialization, add any already registered characteristics to the collection
        for (auto characteristics : RegisteredCharacteristics) {
            AddCharacteristicToCollection(characteristics);
        }
    }

    void Characteristics::Dispose() {
        SongCore::API::Characteristics::GetCharacteristicsUpdatedEvent() -= {&Characteristics::CharacteristicsUpdated, this};
    }

    void Characteristics::RegisterCustomCharacteristic(GlobalNamespace::BeatmapCharacteristicSO* characteristic) {
        return SongCore::API::Characteristics::RegisterCustomCharacteristic(characteristic);
    }

    void Characteristics::UnregisterCustomCharacteristic(GlobalNamespace::BeatmapCharacteristicSO* characteristic) {
        return SongCore::API::Characteristics::UnregisterCustomCharacteristic(characteristic);
    }

    std::span<GlobalNamespace::BeatmapCharacteristicSO*> Characteristics::GetRegisteredCharacteristics() {
        return SongCore::API::Characteristics::GetRegisteredCharacteristics();
    }

    std::span<UnityW<GlobalNamespace::BeatmapCharacteristicSO>> Characteristics::GetEnabledCharacteristics() {
        return _beatmapCharacteristics.ref_to();
    }

    std::span<UnityW<GlobalNamespace::BeatmapCharacteristicSO>> Characteristics::GetDisabledCharacteristics() {
        return _disabledBeatmapCharacteristics.ref_to();
    }

    UnorderedEventCallback<GlobalNamespace::BeatmapCharacteristicSO*, SongCore::API::Characteristics::CharacteristicEventKind>& Characteristics::GetCharacteristicsUpdatedEvent() {
        return _characteristicsUpdated;
    }

    void Characteristics::CharacteristicsUpdated(GlobalNamespace::BeatmapCharacteristicSO* characteristic, SongCore::API::Characteristics::CharacteristicEventKind eventKind) {
        switch (eventKind) {
            using enum SongCore::API::Characteristics::CharacteristicEventKind;
            case Registered:
                AddCharacteristicToCollection(characteristic);
                break;
            case Unregistered:
                RemoveCharacteristicFromCollection(characteristic);
                break;
        }

        _characteristicsUpdated.invoke(characteristic, eventKind);
    }

    void Characteristics::AddCharacteristicToCollection(GlobalNamespace::BeatmapCharacteristicSO* characteristic) {
        std::lock_guard<std::mutex> lock(_collectionMutex);
        auto serializedName = characteristic->serializedName;
        if (characteristic->requires360Movement && !_appStaticSettings->enable360DegreeLevels) {
            _disabledBeatmapCharacteristics->Add(characteristic);
        } else {
            _beatmapCharacteristicCollection->_beatmapCharacteristicsBySerializedName->Add(serializedName, characteristic);
            _beatmapCharacteristics->Add(characteristic);
        }
    }

    void Characteristics::RemoveCharacteristicFromCollection(GlobalNamespace::BeatmapCharacteristicSO* characteristic) {
        std::lock_guard<std::mutex> lock(_collectionMutex);

        auto disabledCharacteristicsIdx = _disabledBeatmapCharacteristics->IndexOf(characteristic);
        if (disabledCharacteristicsIdx >= 0) {
            _disabledBeatmapCharacteristics->RemoveAt(disabledCharacteristicsIdx);
        }

        auto characteristicsIdx = _beatmapCharacteristics->IndexOf(characteristic);
        if (characteristicsIdx >= 0) {
            _beatmapCharacteristics->RemoveAt(disabledCharacteristicsIdx);

            // if the characteristic appears in the regular list, it was also added to the dictionary
            _beatmapCharacteristicCollection->_beatmapCharacteristicsBySerializedName->Remove(characteristic->serializedName);
        }
    }
}
