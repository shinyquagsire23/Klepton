#pragma once

#include "UnityEngine/MonoBehaviour.hpp"
#include "UnityEngine/UI/VerticalLayoutGroup.hpp"
#include "UnityEngine/UI/HorizontalOrVerticalLayoutGroup.hpp"
#include "HMUI/ViewController.hpp"
#include "HMUI/TableView.hpp"
#include "HMUI/TableCell.hpp"
#include "HMUI/ImageView.hpp"
#include "custom-types/shared/macros.hpp"
#include "System/Object.hpp"
#include "TMPro/TextMeshProUGUI.hpp"
#include "song-details/shared/Data/Song.hpp"
#include "song-details/shared/Data/SongDifficulty.hpp"
#include "song-details/shared/Data/MapCharacteristic.hpp"
#include "bsml/shared/macros.hpp"
#include "bsml/shared/BSML.hpp"
#include "bsml/shared/BSML/Components/CustomListTableData.hpp"
#include "songcore/shared/SongLoader/CustomBeatmapLevel.hpp"


DECLARE_CLASS_CODEGEN(BetterSongSearch::UI::ViewControllers, CustomSongListTableCell, HMUI::TableCell) {
    DECLARE_OVERRIDE_METHOD_MATCH(void, SelectionDidChange, &HMUI::SelectableCell::SelectionDidChange, HMUI::SelectableCell::TransitionType transitionType);
    DECLARE_OVERRIDE_METHOD_MATCH(void, HighlightDidChange, &HMUI::SelectableCell::HighlightDidChange, HMUI::SelectableCell::TransitionType transitionType);
    DECLARE_OVERRIDE_METHOD_MATCH(void, WasPreparedForReuse, &HMUI::TableCell::WasPreparedForReuse);
    DECLARE_INSTANCE_FIELD(TMPro::TextMeshProUGUI*, fullFormattedSongName);
    DECLARE_INSTANCE_FIELD(TMPro::TextMeshProUGUI*, uploadDateFormatted);
    DECLARE_INSTANCE_FIELD(TMPro::TextMeshProUGUI*, levelAuthorName);
    DECLARE_INSTANCE_FIELD(TMPro::TextMeshProUGUI*, songLengthAndRating);
    DECLARE_INSTANCE_FIELD(ArrayW<TMPro::TextMeshProUGUI*>, diffs);
    DECLARE_INSTANCE_FIELD(UnityEngine::UI::HorizontalOrVerticalLayoutGroup*, diffsContainer);
    DECLARE_INSTANCE_FIELD(HMUI::ImageView *, bgContainer);

    DECLARE_CTOR(ctor);
    DECLARE_INSTANCE_METHOD(void, OnDestroy);

public:
    CustomSongListTableCell* PopulateWithSongData(const SongDetailsCache::Song* entry);
    const SongDetailsCache::Song* entry;


    void SetFontSizes();
    void RefreshBgState();
    void OnSongsLoaded(std::span<SongCore::SongLoader::CustomBeatmapLevel* const> songs);
};
