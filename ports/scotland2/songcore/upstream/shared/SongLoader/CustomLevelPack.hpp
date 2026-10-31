#pragma once

#include "custom-types/shared/macros.hpp"
#include "GlobalNamespace/BeatmapLevelPack.hpp"
#include "GlobalNamespace/BeatmapLevel.hpp"
#include "CustomBeatmapLevel.hpp"
#include <string>

DECLARE_CLASS_CODEGEN(SongCore::SongLoader, CustomLevelPack, GlobalNamespace::BeatmapLevelPack) {
    DECLARE_CTOR(ctor, StringW packId, StringW packName, UnityEngine::Sprite* coverImage);

    public:
        static CustomLevelPack* New(std::string_view packId, std::string_view packName, UnityEngine::Sprite* coverImage = nullptr);

        /// @brief sorts the levels in the collection, based on `a->songName < b->songName`
        void SortLevels();

        /// @brief sorting function that returns `a < b`
        using WeakSortingFunc = std::function<bool(GlobalNamespace::BeatmapLevel*, GlobalNamespace::BeatmapLevel*)>;

        /// @brief sorts the levels in the collection based on the inputted sort function. std::stable_sort is used
        void SortLevels(WeakSortingFunc sortingFunc);

        /// @brief sets the levels in the collection based on the inputted span
        void SetLevels(std::span<GlobalNamespace::BeatmapLevel* const> levels);

        /// @brief sets the levels in the collection based on the inputted span
        void SetLevels(std::span<CustomBeatmapLevel* const> levels);
};