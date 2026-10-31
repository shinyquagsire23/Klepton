#include "SongLoader/CustomLevelPack.hpp"

#include <compare>
#include <string_view>

DEFINE_TYPE(SongCore::SongLoader, CustomLevelPack);

namespace SongCore::SongLoader {
    void CustomLevelPack::ctor(StringW packId, StringW packName, UnityEngine::Sprite* coverImage) {
        _ctor(packId, packName, packName, coverImage, coverImage, GlobalNamespace::PackBuyOption::DisableBuyOption, ArrayW<GlobalNamespace::BeatmapLevel*>::Empty(), GlobalNamespace::PlayerSensitivityFlag::Unknown);
    }

    CustomLevelPack* CustomLevelPack::New(std::string_view packId, std::string_view packName, UnityEngine::Sprite* coverImage) {
        return CustomLevelPack::New_ctor(packId, packName, coverImage);
    }

    void CustomLevelPack::SortLevels() {
        SortLevels([](auto a, auto b) -> bool { return static_cast<std::u16string_view>(a->songName) < static_cast<std::u16string_view>(b->songName); });
    }

    void CustomLevelPack::SortLevels(WeakSortingFunc sortingFunc) {
        std::stable_sort(_beatmapLevels.begin(), _beatmapLevels.end(), sortingFunc);
    }

    void CustomLevelPack::SetLevels(std::span<CustomBeatmapLevel* const> levels) {
        _beatmapLevels = ArrayW<GlobalNamespace::BeatmapLevel*>(levels.size());
        std::copy(levels.begin(), levels.end(), _beatmapLevels.begin());
        _allBeatmapLevels = ListW<GlobalNamespace::BeatmapLevel*>::New();
        _allBeatmapLevels->AddRange(static_cast<::System::Collections::Generic::IEnumerable_1<GlobalNamespace::BeatmapLevel*>*>(_beatmapLevels.convert()));
        _allBeatmapLevels->AddRange(static_cast<::System::Collections::Generic::IEnumerable_1<GlobalNamespace::BeatmapLevel*>*>(static_cast<void*>(_additionalBeatmapLevels)));
    }

    void CustomLevelPack::SetLevels(std::span<GlobalNamespace::BeatmapLevel* const> levels) {
        _beatmapLevels = ArrayW<GlobalNamespace::BeatmapLevel*>(levels.size());
        std::copy(levels.begin(), levels.end(), _beatmapLevels.begin());
        _allBeatmapLevels = ListW<GlobalNamespace::BeatmapLevel*>::New();
        _allBeatmapLevels->AddRange(static_cast<::System::Collections::Generic::IEnumerable_1<GlobalNamespace::BeatmapLevel*>*>(_beatmapLevels.convert()));
        _allBeatmapLevels->AddRange(static_cast<::System::Collections::Generic::IEnumerable_1<GlobalNamespace::BeatmapLevel*>*>(static_cast<void*>(_additionalBeatmapLevels)));
    }
}
