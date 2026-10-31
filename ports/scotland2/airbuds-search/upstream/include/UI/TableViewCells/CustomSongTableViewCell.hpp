#pragma once

#include "HMUI/ImageView.hpp"
#include "HMUI/TableCell.hpp"
#include "HMUI/TableView.hpp"
#include "HMUI/ViewController.hpp"
#include "System/Object.hpp"
#include "TMPro/TextMeshProUGUI.hpp"
#include "UnityEngine/MonoBehaviour.hpp"
#include "UnityEngine/UI/HorizontalOrVerticalLayoutGroup.hpp"
#include "UnityEngine/UI/VerticalLayoutGroup.hpp"
#include "bsml/shared/BSML.hpp"
#include "bsml/shared/BSML/Components/CustomListTableData.hpp"
#include "bsml/shared/BSML/Components/HotReloadFileWatcher.hpp"
#include "bsml/shared/macros.hpp"
#include "custom-types/shared/macros.hpp"
#include "song-details/shared/SongDetails.hpp"

DECLARE_CLASS_CODEGEN(AirbudsSearch::UI, CustomSongTableViewCell, HMUI::TableCell) {

    DECLARE_CTOR(ctor);

    DECLARE_OVERRIDE_METHOD_MATCH(void, SelectionDidChange, &HMUI::SelectableCell::SelectionDidChange, HMUI::SelectableCell::TransitionType transitionType);
    DECLARE_OVERRIDE_METHOD_MATCH(void, HighlightDidChange, &HMUI::SelectableCell::HighlightDidChange, HMUI::SelectableCell::TransitionType transitionType);
    DECLARE_OVERRIDE_METHOD_MATCH(void, WasPreparedForReuse, &HMUI::TableCell::WasPreparedForReuse);

    DECLARE_INSTANCE_FIELD(HMUI::ImageView*, root_);
    DECLARE_INSTANCE_FIELD(HMUI::ImageView*, image_);
    DECLARE_INSTANCE_FIELD(TMPro::TextMeshProUGUI*, songNameTextView_);
    DECLARE_INSTANCE_FIELD(TMPro::TextMeshProUGUI*, uploaderNameTextView_);
    DECLARE_INSTANCE_FIELD(TMPro::TextMeshProUGUI*, uploadTimeTextView_);
    DECLARE_INSTANCE_FIELD(TMPro::TextMeshProUGUI*, upvoteRatioTextView_);
    DECLARE_INSTANCE_FIELD(UnityEngine::UI::HorizontalLayoutGroup*, diffsContainer_);

    DECLARE_INSTANCE_METHOD(void, OnDestroy);

    public:
    void setSong(const SongDetailsCache::Song* const song);

    private:
    void updateBackground();

    const SongDetailsCache::Song* song_;
};
