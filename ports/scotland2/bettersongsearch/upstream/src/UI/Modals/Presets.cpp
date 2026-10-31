#include "UI/Modals/Presets.hpp"

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
#include "UI/Modals/PresetsTable.hpp"
#include "UnityEngine/Resources.hpp"

using namespace BetterSongSearch::UI;
using namespace BetterSongSearch::Util;
#define coro(coroutine) BSML::SharedCoroutineStarter::get_instance()->StartCoroutine(custom_types::Helpers::CoroutineHelper::New(coroutine))

DEFINE_TYPE(BetterSongSearch::UI::Modals, Presets);

void Modals::Presets::OnEnable() {
}

void Modals::Presets::PostParse() {
    // BSML has a bug that stops getting the correct platform helper and on game reset it dies and the scrollhelper stays invalid and scroll doesn't
    // work
    auto platformHelper = BSML::Helpers::GetDiContainer()->Resolve<GlobalNamespace::IVRPlatformHelper*>();
    if (platformHelper == nullptr) {
    } else {
        for (auto x : this->GetComponentsInChildren<HMUI::ScrollView*>()) {
            x->____platformHelper = platformHelper;
        }
    }

    if (this->presetListTableData) {
        INFO("Table exists");
        this->presetListTableData->tableView->SetDataSource(reinterpret_cast<HMUI::TableView::IDataSource*>(this), false);
    }

    // BSML / HMUI my beloved (this currently doesn't work on quest since it was stripped from the build, bsml needs a fix)
    newPresetNameSetting->modalKeyboard->modalView->_animateParentCanvas = false;
}

void Modals::Presets::CloseModal() {
    this->presetsModal->Hide();
}

void Modals::Presets::ctor() {
    INVOKE_CTOR();
    this->initialized = false;
}

void Modals::Presets::OpenModal() {
    if (!initialized) {
        BSML::parse_and_construct(Assets::Presets_bsml, this->get_transform(), this);
        initialized = true;
    }
    RefreshPresetsList();
    this->presetsModal->Show();
}

void Modals::Presets::OpenSavePresetModal() {
    if (!initialized) {
        BSML::parse_and_construct(Assets::Presets_bsml, this->get_transform(), this);
        initialized = true;
    }
    this->savePresetModal->Show();
}

void Modals::Presets::CloseSavePresetModal() {
    this->savePresetModal->Hide();
}

// Table stuff
HMUI::TableCell* Modals::Presets::CellForIdx(HMUI::TableView* tableView, int idx) {
    return Modals::PresetsTableCell::GetCell(tableView)->PopulateWithPresetName(presets[idx]);
}

float Modals::Presets::CellSize() {
    return 7.0f;
}

int Modals::Presets::NumberOfCells() {
    return presets.size();
}

void Modals::Presets::AddPreset() {
    try {
        std::string newPresetName = newPresetNameSetting->get_text();
        if (newPresetName == "") {
            return;
        }
        dataHolder.filterOptions.SaveToPreset(newPresetName);

        presets.push_back(newPresetName);

    } catch (std::exception& e) {
        ERROR("Failed to save preset: {}", e.what());
    }

    RefreshPresetsList();
    CloseSavePresetModal();
}

void Modals::Presets::LoadPreset() {
    if (selectedPreset == "") {
        return;
    }

    auto presetOptional = dataHolder.filterOptions.LoadFromPreset(selectedPreset);

    if (!presetOptional.has_value()) {
        return;
    }

    dataHolder.filterOptions = presetOptional.value();
    dataHolder.filterOptions.SaveToConfig();

    // Update filter settings
    fcInstance->FilterViewController->UpdateLocalState();
    fcInstance->FilterViewController->ForceRefreshUI();

    DEBUG("Filters changed");

    // Update filter options state
    auto slController = fcInstance->SongListController;
    slController->SortAndFilterSongs(dataHolder.sort, dataHolder.search, true);

    CloseModal();
}

void Modals::Presets::DeletePreset() {
    if (selectedPreset == "") {
        return;
    }
    dataHolder.filterOptions.DeletePreset(selectedPreset);
    RefreshPresetsList();

    if (presetListTableData && presetListTableData->tableView) {
        presetListTableData->tableView->ClearSelection();
    }
    selectedPreset = "";
}

void Modals::Presets::PresetSelected(UnityW<HMUI::TableView> table, int id) {
    if (id < 0 || id >= presets.size()) {
        return;
    }

    loadButton->set_interactable(true);
    deleteButton->set_interactable(true);

    selectedPreset = presets[id];

    newPresetNameSetting->set_text(selectedPreset);
}

void Modals::Presets::RefreshPresetsList() {
    auto presetList = FilterProfile::GetPresetList();
    presets.clear();

    for (auto& preset : presetList) {
        presets.push_back(preset);
    }

    if (presetListTableData && presetListTableData->tableView) {
        presetListTableData->tableView->ReloadData();
    }

    loadButton->set_interactable(false);
    deleteButton->set_interactable(false);

    newPresetNameSetting->set_text("");
    selectedPreset = "";
}
