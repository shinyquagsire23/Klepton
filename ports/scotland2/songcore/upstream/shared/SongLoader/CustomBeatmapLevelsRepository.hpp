#pragma once

#include "custom-types/shared/macros.hpp"
#include "GlobalNamespace/BeatmapLevelsRepository.hpp"
#include "CustomLevelPack.hpp"
#include <span>

DECLARE_CLASS_CODEGEN(SongCore::SongLoader, CustomBeatmapLevelsRepository, GlobalNamespace::BeatmapLevelsRepository) {
    DECLARE_CTOR(ctor);
        DECLARE_INSTANCE_FIELD_PRIVATE(ListW<GlobalNamespace::BeatmapLevelPack*>, _levelPacks);
    public:
        /// @brief adds a level pack to the level packs list
        void AddLevelPack(GlobalNamespace::BeatmapLevelPack* pack);
        /// @brief removes a level pack from the level packs list
        void RemoveLevelPack(GlobalNamespace::BeatmapLevelPack* pack);
        /// @brief completely clears the level packs list
        void ClearLevelPacks();
        /// @brief takes the level packs list and fixes all the backing dictionaries. This is a somewhat expensive operation as it involves a lot of inserts into dicts
        void FixBackingDictionaries();

        std::span<GlobalNamespace::BeatmapLevelPack* const> GetBeatmapLevelPacks() const { return _levelPacks; }
        __declspec(property(get=GetBeatmapLevelPacks)) std::span<GlobalNamespace::BeatmapLevelPack* const> BeatmapLevelPacks;
};