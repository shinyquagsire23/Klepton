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
#include "bsml/shared/macros.hpp"
#include "custom-types/shared/macros.hpp"
#include "song-details/shared/SongDetails.hpp"

#include "Airbuds/Playlist.hpp"

DECLARE_CLASS_CODEGEN(AirbudsSearch::UI, AirbudsPlaylistTableViewCell, HMUI::TableCell) {

    DECLARE_CTOR(ctor);

    DECLARE_OVERRIDE_METHOD_MATCH(void, SelectionDidChange, &HMUI::SelectableCell::SelectionDidChange, HMUI::SelectableCell::TransitionType transitionType);
    DECLARE_OVERRIDE_METHOD_MATCH(void, HighlightDidChange, &HMUI::SelectableCell::HighlightDidChange, HMUI::SelectableCell::TransitionType transitionType);
    DECLARE_OVERRIDE_METHOD_MATCH(void, WasPreparedForReuse, &HMUI::TableCell::WasPreparedForReuse);

    DECLARE_INSTANCE_FIELD(HMUI::ImageView*, root_);
    DECLARE_INSTANCE_FIELD(HMUI::ImageView*, image_);
    DECLARE_INSTANCE_FIELD(TMPro::TextMeshProUGUI*, playlistNameTextView_);
    DECLARE_INSTANCE_FIELD(TMPro::TextMeshProUGUI*, songCountTextView_);

    DECLARE_INSTANCE_METHOD(void, OnDestroy);

    public:
    static constexpr std::string_view CELL_REUSE_ID = "AirbudsPlaylistTableViewCell";
    void setPlaylist(const airbuds::Playlist& playlist);

    private:
    void updateBackground();

    std::unique_ptr<const airbuds::Playlist> playlist_;
};
