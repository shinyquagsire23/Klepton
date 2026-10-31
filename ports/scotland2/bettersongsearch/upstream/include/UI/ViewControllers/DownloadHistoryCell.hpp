#pragma once

#include "DownloadHistory.hpp"
#include "UnityEngine/MonoBehaviour.hpp"
#include "UnityEngine/UI/VerticalLayoutGroup.hpp"
#include "custom-types/shared/macros.hpp"
#include "HMUI/ViewController.hpp"
#include "HMUI/TableView.hpp"
#include "HMUI/TableCell.hpp"
#include "System/Object.hpp"
#include "TMPro/TextMeshProUGUI.hpp"
#include "song-details/shared/Data/Song.hpp"
#include "bsml/shared/macros.hpp"
#include "bsml/shared/BSML.hpp"
#include "bsml/shared/BSML/Components/CustomListTableData.hpp"


DECLARE_CLASS_CODEGEN(BetterSongSearch::UI::ViewControllers, CustomDownloadListTableCell, HMUI::TableCell) {
    DECLARE_OVERRIDE_METHOD_MATCH(void, SelectionDidChange, &HMUI::SelectableCell::SelectionDidChange, HMUI::SelectableCell::TransitionType transitionType);
    DECLARE_OVERRIDE_METHOD_MATCH(void, HighlightDidChange, &HMUI::SelectableCell::HighlightDidChange, HMUI::SelectableCell::TransitionType transitionType);
    DECLARE_OVERRIDE_METHOD_MATCH(void, WasPreparedForReuse, &HMUI::TableCell::WasPreparedForReuse);
    DECLARE_INSTANCE_FIELD(HMUI::ImageView*, bgContainer);
    DECLARE_INSTANCE_FIELD(HMUI::ImageView*, bgProgress);
    DECLARE_INSTANCE_FIELD(TMPro::TextMeshProUGUI*, songName);
    DECLARE_INSTANCE_FIELD(TMPro::TextMeshProUGUI*, levelAuthorName);
    DECLARE_INSTANCE_FIELD(TMPro::TextMeshProUGUI*, statusLabel);

public:
    DownloadHistoryEntry* entry;

    CustomDownloadListTableCell* PopulateWithSongData(DownloadHistoryEntry* entry);
    void UpdateProgress();
    void RefreshBar();
    void RefreshBgState();
};
