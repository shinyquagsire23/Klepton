#pragma once

#include "UnityEngine/MonoBehaviour.hpp"
#include "UnityEngine/UI/VerticalLayoutGroup.hpp"
#include "HMUI/ViewController.hpp"
#include "HMUI/TableCell.hpp"
#include "HMUI/ImageView.hpp"
#include "custom-types/shared/macros.hpp"
#include "System/Object.hpp"
#include "TMPro/TextMeshProUGUI.hpp"
#include "bsml/shared/macros.hpp"
#include "bsml/shared/BSML.hpp"
#include "bsml/shared/BSML/Components/CustomListTableData.hpp"
#include "HMUI/Touchable.hpp"
#include "HMUI/TableView.hpp"
#include "UnityEngine/UI/HorizontalOrVerticalLayoutGroup.hpp"
#include "assets.hpp"

DECLARE_CLASS_CODEGEN(BetterSongSearch::UI::Modals, PresetsTableCell, HMUI::TableCell) {
    DECLARE_OVERRIDE_METHOD_MATCH(void, SelectionDidChange, &HMUI::SelectableCell::SelectionDidChange, HMUI::SelectableCell::TransitionType transitionType);
    DECLARE_OVERRIDE_METHOD_MATCH(void, HighlightDidChange, &HMUI::SelectableCell::HighlightDidChange, HMUI::SelectableCell::TransitionType transitionType);
    DECLARE_OVERRIDE_METHOD_MATCH(void, WasPreparedForReuse, &HMUI::TableCell::WasPreparedForReuse);
    DECLARE_INSTANCE_FIELD(TMPro::TextMeshProUGUI*, presetNameLabel);
    DECLARE_INSTANCE_FIELD(HMUI::ImageView*, bgContainer);

public:
    PresetsTableCell* PopulateWithPresetName(StringW presetName);
    static PresetsTableCell *GetCell(HMUI::TableView *tableView);

    private:
        void RefreshBgState();
};
