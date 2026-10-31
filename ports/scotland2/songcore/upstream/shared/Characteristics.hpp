#pragma once

#include "custom-types/shared/macros.hpp"
#include "./SongCore.hpp"

#include "GlobalNamespace/MainSystemInit.hpp"
#include "GlobalNamespace/BeatmapCharacteristicCollection.hpp"
#include "GlobalNamespace/BeatmapCharacteristicSO.hpp"
#include "GlobalNamespace/AppStaticSettingsSO.hpp"
#include "Zenject/IInitializable.hpp"
#include "System/IDisposable.hpp"

DECLARE_CLASS_CODEGEN_INTERFACES(SongCore, Characteristics, System::Object, Zenject::IInitializable*, System::IDisposable*) {
        DECLARE_OVERRIDE_METHOD_MATCH(void, Initialize, &Zenject::IInitializable::Initialize);
        DECLARE_OVERRIDE_METHOD_MATCH(void, Dispose, &System::IDisposable::Dispose);

        DECLARE_INSTANCE_FIELD_PRIVATE(ListW<UnityW<GlobalNamespace::BeatmapCharacteristicSO>>, _beatmapCharacteristics);
        DECLARE_INSTANCE_FIELD_PRIVATE(ListW<UnityW<GlobalNamespace::BeatmapCharacteristicSO>>, _disabledBeatmapCharacteristics);
        DECLARE_INSTANCE_FIELD_PRIVATE(GlobalNamespace::BeatmapCharacteristicCollection*, _beatmapCharacteristicCollection);
        DECLARE_INSTANCE_FIELD_PRIVATE(GlobalNamespace::AppStaticSettingsSO*, _appStaticSettings);

        DECLARE_CTOR(ctor, GlobalNamespace::BeatmapCharacteristicCollection* beatmapCharacteristicCollection, GlobalNamespace::AppStaticSettingsSO* appStaticSettings);
    public:
        /// @brief Registers a custom characteristic
        void RegisterCustomCharacteristic(GlobalNamespace::BeatmapCharacteristicSO* characteristic);

        /// @brief Unregisters a custom characteristic
        void UnregisterCustomCharacteristic(GlobalNamespace::BeatmapCharacteristicSO* characteristic);

        /// @brief provides access to the registered characteristics without allowing edits
        std::span<GlobalNamespace::BeatmapCharacteristicSO*> GetRegisteredCharacteristics();
        __declspec(property(get=GetRegisteredCharacteristics)) std::span<GlobalNamespace::BeatmapCharacteristicSO*> RegisteredCharacteristics;

        /// @brief provides access to an event that gets invoked when the custom characteristics are updated. not guaranteed to run on main thread! cleared on soft restart. Invoked after the particular characteristic is added to the list.
        UnorderedEventCallback<GlobalNamespace::BeatmapCharacteristicSO*, SongCore::API::Characteristics::CharacteristicEventKind>& GetCharacteristicsUpdatedEvent();
        __declspec(property(get=GetCharacteristicsUpdatedEvent)) UnorderedEventCallback<GlobalNamespace::BeatmapCharacteristicSO*, SongCore::API::Characteristics::CharacteristicEventKind>& CharacteristicsUpdatedEvent;

        /// @brief provides access to enabled characteristics
        std::span<UnityW<GlobalNamespace::BeatmapCharacteristicSO>> GetEnabledCharacteristics();
        __declspec(property(get=GetEnabledCharacteristics)) std::span<UnityW<GlobalNamespace::BeatmapCharacteristicSO>> EnabledCharacteristics;

        /// @brief provides access to disabled characteristics (require 360 while the app settings say no 360)
        std::span<UnityW<GlobalNamespace::BeatmapCharacteristicSO>> GetDisabledCharacteristics();
        __declspec(property(get=GetDisabledCharacteristics)) std::span<UnityW<GlobalNamespace::BeatmapCharacteristicSO>> DisabledCharacteristics;
    private:
        /// @brief adds the characteristic to the relevant collections
        void AddCharacteristicToCollection(GlobalNamespace::BeatmapCharacteristicSO* characteristic);

        /// @brief removes the characteristic to the relevant collections
        void RemoveCharacteristicFromCollection(GlobalNamespace::BeatmapCharacteristicSO* characteristic);

        /// @brief callback ran when the songcore api is used to update characteristics
        void CharacteristicsUpdated(GlobalNamespace::BeatmapCharacteristicSO* characteristic, SongCore::API::Characteristics::CharacteristicEventKind eventKind);

        UnorderedEventCallback<GlobalNamespace::BeatmapCharacteristicSO*, SongCore::API::Characteristics::CharacteristicEventKind> _characteristicsUpdated;
        std::mutex _collectionMutex;
};