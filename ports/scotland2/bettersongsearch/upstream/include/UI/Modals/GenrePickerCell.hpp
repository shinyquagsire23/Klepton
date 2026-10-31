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
#include "bsml/shared/BSML/Components/ClickableText.hpp"
#include "HMUI/Touchable.hpp"
#include "HMUI/TableView.hpp"
#include "UnityEngine/UI/HorizontalOrVerticalLayoutGroup.hpp"
#include "assets.hpp"
#include "UI/Modals/GenrePicker.hpp"

DECLARE_CLASS_CODEGEN(BetterSongSearch::UI::Modals, GenrePickerCell, HMUI::TableCell) {
    DECLARE_OVERRIDE_METHOD_MATCH(void, SelectionDidChange, &HMUI::SelectableCell::SelectionDidChange, HMUI::SelectableCell::TransitionType transitionType);
    DECLARE_OVERRIDE_METHOD_MATCH(void, HighlightDidChange, &HMUI::SelectableCell::HighlightDidChange, HMUI::SelectableCell::TransitionType transitionType);
    DECLARE_OVERRIDE_METHOD_MATCH(void, WasPreparedForReuse, &HMUI::TableCell::WasPreparedForReuse);

    DECLARE_INSTANCE_FIELD(UnityW<BSML::ClickableText>, includeButton);
    DECLARE_INSTANCE_FIELD(UnityW<BSML::ClickableText>, excludeButton);

    DECLARE_INSTANCE_METHOD(void, IncludeGenre);
    DECLARE_INSTANCE_METHOD(void, ExcludeGenre);

    DECLARE_INSTANCE_METHOD(void, Refresh);

public:
    BetterSongSearch::UI::Modals::GenrePickerCell* PopulateWithGenre(BetterSongSearch::UI::Modals::GenreCellState* state);
    static BetterSongSearch::UI::Modals::GenrePickerCell *GetCell(HMUI::TableView *tableView);

private:
    BetterSongSearch::UI::Modals::GenreCellState* genre;
};
