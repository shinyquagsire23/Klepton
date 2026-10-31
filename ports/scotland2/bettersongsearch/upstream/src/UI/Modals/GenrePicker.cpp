#include "UI/Modals/GenrePicker.hpp"

#include "assets.hpp"
#include "bsml/shared/BSML.hpp"
#include "bsml/shared/Helpers/getters.hpp"
#include "DataHolder.hpp"
#include "FilterOptions.hpp"
#include "GlobalNamespace/LevelCollectionTableView.hpp"
#include "HMUI/TableView.hpp"
#include "logging.hpp"
#include "System/Tuple_2.hpp"
#include "UI/FlowCoordinators/BetterSongSearchFlowCoordinator.hpp"
#include "UI/Modals/GenrePickerCell.hpp"
#include "UnityEngine/Resources.hpp"
#include "Util/TextUtil.hpp"

using namespace BetterSongSearch::UI;
using namespace BetterSongSearch::Util;
#define coro(coroutine) BSML::SharedCoroutineStarter::get_instance()->StartCoroutine(custom_types::Helpers::CoroutineHelper::New(coroutine))

DEFINE_TYPE(BetterSongSearch::UI::Modals, GenrePicker);

void Modals::GenrePicker::OnEnable() {
}

void Modals::GenrePicker::PostParse() {
    // BSML has a bug that stops getting the correct platform helper and on game reset it dies and the scrollhelper stays invalid and scroll doesn't
    // TODO: Move to getting it from BSML when the patches are merged and released
    auto platformHelper = BSML::Helpers::GetDiContainer()->Resolve<GlobalNamespace::IVRPlatformHelper*>();
    if (platformHelper == nullptr) {
    } else {
        for (auto x : this->GetComponentsInChildren<HMUI::ScrollView*>()) {
            x->____platformHelper = platformHelper;
        }
    }
}

void Modals::GenrePicker::CloseModal() {
    this->genrePickerModal->Hide();
}

void Modals::GenrePicker::ctor() {
    INVOKE_CTOR();
    this->initialized = false;

    // Subscribe to events
    dataHolder.loadingFinished += {&Modals::GenrePicker::RefreshGenreList, this};
}

void Modals::GenrePicker::OnDestroy() {
    // Unsub from events
    dataHolder.loadingFinished -= {&Modals::GenrePicker::RefreshGenreList, this};
}

void Modals::GenrePicker::RefreshGenreList() {
    if (!genresTableData) {
        return;
    }
    auto table = genresTableData->tableView;
    if (!table) {
        return;
    }

    DEBUG("Refreshing genre list");

    DEBUG("Genre list size: {}", dataHolder.tags.size());

    if (dataHolder.tags.size() == 0) {
        return;
    }

    // Recalculate preprocessed values
    dataHolder.filterOptions.RecalculatePreprocessedValues();

    std::vector<GenreCellState> tempGenres;
    for (auto& genre : dataHolder.tags) {
        GenreCellStatus state = GenreCellStatus::None;
        auto mask = genre.mask;

        auto isExcluded = dataHolder.filterOptions._mapGenreExcludeBitfield & mask;
        auto isIncluded = dataHolder.filterOptions._mapGenreBitfield & mask;

        if (isExcluded) {
            state = GenreCellStatus::Exclude;
        }
        if (isIncluded) {
            state = GenreCellStatus::Include;
        }

        tempGenres.push_back({genre.tag, mask, state, genre.songCount});
    }

    DEBUG("Genre list size: {}", tempGenres.size());

    // Swap the vectors
    tempGenres.swap(genres);

    DEBUG("Genre list refreshed, {} genres", genres.size());

    table->SetDataSource(reinterpret_cast<HMUI::TableView::IDataSource*>(this), false);
    table->ReloadData();
}

void Modals::GenrePicker::OpenModal() {
    if (!initialized) {
        BSML::parse_and_construct(Assets::GenrePicker_bsml, this->get_transform(), this);
        initialized = true;
    }

    INFO("Opening genre picker modal");
    this->genrePickerModal->Show();

    if (!dataHolder.songDetails->tags.get_isDataAvailable()) {
        return;
    }

    auto fv = fcInstance->FilterViewController;
    if (!fv) {
        return;
    }

    dataHolder.PreprocessTags();
    RefreshGenreList();
}

void Modals::GenrePicker::ClearGenre() {
    dataHolder.filterOptions.mapGenreString = "";
    dataHolder.filterOptions.mapGenreExcludeString = "";
    getPluginConfig().MapGenreExcludeString.SetValue("");
    getPluginConfig().MapGenreString.SetValue("");

    // Trigger refresh of songs
    auto slController = fcInstance->SongListController;
    slController->SortAndFilterSongs(dataHolder.sort, dataHolder.search, true);

    // Update filter settings
    fcInstance->FilterViewController->UpdateLocalState();
    fcInstance->FilterViewController->ForceRefreshUI();

    this->genrePickerModal->Hide();
}

// Table stuff
HMUI::TableCell* Modals::GenrePicker::CellForIdx(HMUI::TableView* tableView, int idx) {
    return Modals::GenrePickerCell::GetCell(tableView)->PopulateWithGenre(&genres[idx]);
}

float Modals::GenrePicker::CellSize() {
    return 5.00f;
}

int Modals::GenrePicker::NumberOfCells() {
    return genres.size();
}

void Modals::GenrePicker::SelectGenre() {
    std::vector<std::string> selectedGenres;
    std::vector<std::string> excludedGenres;

    for (auto& g : genres) {
        if (g.status == GenreCellStatus::Include) {
            selectedGenres.push_back(g.tag);
        } else if (g.status == GenreCellStatus::Exclude) {
            excludedGenres.push_back(g.tag);
        }
    }

    // Trigger refresh of songs
    std::string includeString = join(selectedGenres, " ");
    std::string excludeString = join(excludedGenres, " ");

    dataHolder.filterOptions.mapGenreString = includeString;
    dataHolder.filterOptions.mapGenreExcludeString = excludeString;
    getPluginConfig().MapGenreString.SetValue(includeString);
    getPluginConfig().MapGenreExcludeString.SetValue(excludeString);

    auto slController = fcInstance->SongListController;
    slController->SortAndFilterSongs(dataHolder.sort, dataHolder.search, true);

    // Update filter settings
    fcInstance->FilterViewController->UpdateLocalState();
    fcInstance->FilterViewController->ForceRefreshUI();

    this->genrePickerModal->Hide();
}
