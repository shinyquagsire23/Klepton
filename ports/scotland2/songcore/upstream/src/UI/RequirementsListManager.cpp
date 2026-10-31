#include "UI/RequirementsListManager.hpp"

#include "CustomJSONData.hpp"
#include "GlobalNamespace/StandardLevelDetailView.hpp"
#include "GlobalNamespace/EnvironmentInfoSO.hpp"
#include "UnityEngine/Transform.hpp"
#include "UnityEngine/RectTransform.hpp"

#include "logging.hpp"
#include "config.hpp"
#include "assets.hpp"
#include "bsml/shared/BSML.hpp"
#include "bsml/shared/Helpers/delegates.hpp"

DEFINE_TYPE(SongCore::UI, RequirementsListManager);

static inline UnityEngine::Vector3 operator*(UnityEngine::Vector3 vec, float v) {
    return {
        vec.x * v,
        vec.y * v,
        vec.z * v
    };
}

namespace SongCore::UI {
    void RequirementsListManager::ctor(GlobalNamespace::StandardLevelDetailViewController* levelDetailViewController, ColorsOptions* colorsOptions, Capabilities* capabilities, LevelSelect* levelSelect, PlayButtonInteractable* playButtonInteractable, IconCache* iconCache) {
        INVOKE_CTOR();
        _levelDetailViewController = levelDetailViewController;
        _colorsOptions = colorsOptions;
        _capabilities = capabilities;
        _levelSelect = levelSelect;
        _playButtonInteractable = playButtonInteractable;
        _iconCache = iconCache;

        _displayCells = ListW<BSML::CustomCellInfo*>::New();
        _levelInfoCells = ListW<BSML::CustomCellInfo*>::New();
        _disablingModInfoCells = ListW<BSML::CustomCellInfo*>::New();
        _unusedCells = ListW<BSML::CustomCellInfo*>::New();

        _showColorsOnModalHideAction = BSML::MakeSystemAction<>(std::bind(&RequirementsListManager::ShowColorsOptions, this));
    }

    void RequirementsListManager::Initialize() {
        _levelSelect->LevelWasSelected += {&RequirementsListManager::LevelWasSelected, this};
        _playButtonInteractable->PlayButtonDisablingModsChanged += {&RequirementsListManager::PlayButtonDisablingModsChanged, this};
        auto levelDetailView = _levelDetailViewController->_standardLevelDetailView;
        auto toggleRect = levelDetailView->_favoriteToggle->transform.cast<UnityEngine::RectTransform>();

        BSML::parse_and_construct("<bg><action-button id='_requirementButton' text='?' anchor-pos-x='43' anchor-pos-y='10' pref-width='12' pref-height='9' on-click='ShowRequirements'/></bg>", toggleRect->parent, this);
        _requirementButton->transform->parent->localScale *= 0.7f;
        _colorsOptions->onModalHide = std::bind(&RequirementsListManager::ShowRequirements, this);

        toggleRect->anchoredPosition = {3, -8};
        toggleRect->sizeDelta = {10, 5};

        PlayButtonDisablingModsChanged(_playButtonInteractable->PlayButtonDisablingModInfos);
    }

    void RequirementsListManager::Dispose() {
        _levelSelect->LevelWasSelected -= {&RequirementsListManager::LevelWasSelected, this};
        _playButtonInteractable->PlayButtonDisablingModsChanged -= {&RequirementsListManager::PlayButtonDisablingModsChanged, this};
    }

    void RequirementsListManager::UpdateRequirementButton() {
        _requirementButton->gameObject->SetActive(!_displayCells.empty());
    }

    BSML::CustomCellInfo* RequirementsListManager::GetCellInfo() {
        if (_unusedCells.empty()) return BSML::CustomCellInfo::New_ctor();

        auto lastIdx = _unusedCells.size() - 1;
        auto last = _unusedCells[lastIdx];
        _unusedCells->Remove(last);
        return last;
    }

    void RequirementsListManager::ClearCells(ListW<BSML::CustomCellInfo*> list) {
        for (auto cellInfo : list) _unusedCells->Add(cellInfo);
        list->Clear();
    }

    void RequirementsListManager::UpdateDisplayCells() {
        _displayCells.clear();
        _displayCells.insert_range(_levelInfoCells);
        _displayCells.insert_range(_disablingModInfoCells);
    }


    void RequirementsListManager::UpdateRequirementCells(LevelSelect::LevelWasSelectedEventArgs const& eventArgs) {
        DEBUG("Updating requirement cells");
        ClearCells(_levelInfoCells);

        if (!eventArgs.isCustom) {
            DEBUG("Last selected level was not custom! returning...");
            return;
        }

        if (!eventArgs.customLevelDetails.has_value()) {
            DEBUG("Last level selected was custom, but had no level details! returning...");
            return;
        }

        auto& levelDetails = eventArgs.customLevelDetails->levelDetails;
        auto& characteristicDetails = eventArgs.customLevelDetails->characteristicDetails;
        auto& diffDetails = eventArgs.customLevelDetails->difficultyDetails;

        for (auto& requirement : diffDetails.requirements) {
            static ConstString RequirementFound("Requirement Found");
            static ConstString RequirementMissing("Requirement Missing");

            bool installed = _capabilities->IsCapabilityRegistered(requirement);
            auto cell = GetCellInfo();
            cell->text = fmt::format("<size=75%>{}", requirement);
            cell->subText = installed ? RequirementFound : RequirementMissing;
            cell->icon = installed ? _iconCache->HaveReqIcon : _iconCache->MissingReqIcon;
            _levelInfoCells.push_back(cell);
        }

        for (auto& contributor : levelDetails.contributors) {
            auto cell = GetCellInfo();
            
            cell->text = fmt::format("<size=75%>{}", contributor.name);
            cell->subText = contributor.role;
            UnityEngine::Sprite* icon = nullptr;
            if (!contributor.iconPath.empty()) {
                std::filesystem::path levelPath(eventArgs.customBeatmapLevel->customLevelPath);
                icon = _iconCache->GetIconForPath(levelPath / contributor.iconPath);
            }
            cell->icon = icon ? icon : _iconCache->InfoIcon;
            _levelInfoCells.push_back(cell);
        }

        if (eventArgs.isWIP) {
            static ConstString WipText("<size=75%>WIP Level. Please Play in Practice Mode");
            static ConstString WipSubText("Warning");

            auto cell = GetCellInfo();
            cell->text = WipText;
            cell->subText = WipSubText;
            cell->icon = _iconCache->WarningIcon;
            _levelInfoCells.push_back(cell);
        }

        if (diffDetails.customColors.has_value()) {
            static ConstString CustomColorsText("<size=75%>Custom Colors Available");
            static ConstString CustomColorsSubText("Click here to preview & enable or disable it.");

            auto cell = GetCellInfo();
            cell->text = CustomColorsText;
            cell->subText = CustomColorsSubText;
            cell->icon = _iconCache->ColorsIcon;
            _levelInfoCells.push_back(cell);

            _colorsOptions->Parse();
            _colorsOptions->SetColors(diffDetails.customColors.value());
        }

        for (auto& warning : diffDetails.warnings) {
            static ConstString Warning("Warning");

            auto cell = GetCellInfo();
            cell->text = fmt::format("<size=75%>{}", warning);
            cell->subText = Warning;
            cell->icon = _iconCache->WarningIcon;
            _levelInfoCells.push_back(cell);
        }

        for (auto& information : diffDetails.information) {
            static ConstString Info("Info");

            auto cell = GetCellInfo();
            cell->text = fmt::format("<size=75%>{}", information);
            cell->subText = Info;
            cell->icon = _iconCache->InfoIcon;
            _levelInfoCells.push_back(cell);
        }

        for (auto& suggestion : diffDetails.suggestions) {
            static ConstString SuggestionFound("Suggestion Found");
            static ConstString SuggestionMissing("Suggestion Missing");

            bool installed = _capabilities->IsCapabilityRegistered(suggestion);
            auto cell = GetCellInfo();
            cell->text = suggestion;
            cell->subText = installed ? SuggestionFound : SuggestionMissing;
            cell->icon = installed ? _iconCache->HaveSuggestionIcon : _iconCache->MissingSuggestionIcon;
            _levelInfoCells.push_back(cell);
        }

        if (diffDetails.oneSaber.has_value()) {
            auto oneSaber = diffDetails.oneSaber.value();
            static const char* FANCY_ENABLED("[<color=#89ff89>Enabled</color]");
            static const char* FANCY_DISABLED("[<color=#ff5072>Disabled</color]");
            static const char* ENABLE("enable");
            static const char* DISABLE("disable");
            static const char* FORCED_ONE("Forced One Saber");
            static const char* FORCED_TWO("Forced Standard");

            auto enabledText = config.disableOneSaberOverride ? FANCY_DISABLED : FANCY_ENABLED;
            auto enableSubText = config.disableOneSaberOverride ? ENABLE : DISABLE;
            auto saberCountText = oneSaber ? FORCED_ONE : FORCED_TWO;

            auto cell = GetCellInfo();
            cell->text = fmt::format("<size=75%>{} {}", saberCountText, enabledText);
            cell->subText = fmt::format("Map changes saber count, click here to {}", enableSubText);
            cell->icon = oneSaber ? _iconCache->OneSaberIcon : _iconCache->StandardIcon;
            _levelInfoCells.push_back(cell);
        }

        // add environment info here if list isn't empty
        if (!_levelInfoCells.empty()) {
            static ConstString EnvironmentInfo("<size=75%>Environment");

            auto cell = GetCellInfo();
            cell->text = EnvironmentInfo;
            cell->subText = eventArgs.customBeatmapLevel->GetEnvironmentName(eventArgs.beatmapKey.beatmapCharacteristic, eventArgs.beatmapKey.difficulty)._environmentName;
            cell->icon = _iconCache->EnvironmentIcon;
            _levelInfoCells.push_back(cell);
        }
    }

    void RequirementsListManager::UpdateDisablingModInfoCells(std::span<PlayButtonInteractable::PlayButtonDisablingModInfo const> disablingModInfos) {
        ClearCells(_disablingModInfoCells);
        if (disablingModInfos.empty()) return;

        auto cell = GetCellInfo();
        cell->text = fmt::format("Play button disabled");
        cell->icon = _iconCache->WarningIcon;
        cell->subText = "The following mods are responsible:";
        _disablingModInfoCells.push_back(cell);

        for (auto& modInfo : disablingModInfos) {
            static auto NoReason = ConstString("No reason given");
            auto cell = GetCellInfo();
            cell->text = modInfo.modID;
            cell->icon = _iconCache->WarningIcon;
            cell->subText = modInfo.reason.empty() ? NoReason : StringW(modInfo.reason);
            _disablingModInfoCells.push_back(cell);
        }
    }

    void RequirementsListManager::ShowRequirements() {
        if (!_requirementModal) {
            BSML::parse_and_construct(Assets::requirements_bsml, _requirementButton->transform, this);
            _modalPosition = _requirementModal->transform->position;
        }

        _listTableData->tableView->ReloadData();
        _listTableData->tableView->ScrollToCellWithIdx(0, ::HMUI::TableView::ScrollPositionType::Beginning, true);

        _requirementModal->onHide = nullptr;
        _requirementModal->transform->position = _modalPosition;
        _requirementModal->Show();
    }

    void RequirementsListManager::ShowColorsOptions() {
        _colorsOptions->Show();
    }

    void RequirementsListManager::RequirementCellSelected(HMUI::TableView* tableView, int cellIndex) {
        auto cell = _displayCells[cellIndex];
        auto selectedIcon = cell->icon;

        if (selectedIcon == _iconCache->ColorsIcon) {
            _requirementModal->HMUI::ModalView::Hide(false, _showColorsOnModalHideAction);
        } else if (selectedIcon == _iconCache->StandardIcon || selectedIcon == _iconCache->OneSaberIcon) {
            config.disableOneSaberOverride = !config.disableOneSaberOverride;
            SaveConfig();
            _requirementModal->Hide();
        }
        _listTableData->tableView->ClearSelection();
    }

    void RequirementsListManager::LevelWasSelected(LevelSelect::LevelWasSelectedEventArgs const& eventArgs) {
        UpdateRequirementCells(eventArgs);
        UpdateDisplayCells();
        UpdateRequirementButton();
    }

    void RequirementsListManager::PlayButtonDisablingModsChanged(std::span<PlayButtonInteractable::PlayButtonDisablingModInfo const> disablingModInfos) {
        UpdateDisablingModInfoCells(disablingModInfos);
        UpdateDisplayCells();
        UpdateRequirementButton();
    }

    ListW<BSML::CustomCellInfo*> RequirementsListManager::get_requirementsCells() {
        return _displayCells;
    }
}
