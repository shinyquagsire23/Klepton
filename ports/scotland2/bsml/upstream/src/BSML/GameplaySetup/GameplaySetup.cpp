#include "BSML/GameplaySetup/GameplaySetup.hpp"

#include "Helpers/getters.hpp"
#include "assets.hpp"
#include "logging.hpp"
#include "BSML.hpp"

#include "UnityEngine/Resources.hpp"
#include "UnityEngine/Vector2.hpp"
#include "UnityEngine/RectTransform.hpp"
#include "GlobalNamespace/SinglePlayerLevelSelectionFlowCoordinator.hpp"
#include "GlobalNamespace/CampaignFlowCoordinator.hpp"
#include "GlobalNamespace/GameServerLobbyFlowCoordinator.hpp"


DEFINE_TYPE(BSML, GameplaySetup);

namespace BSML {
    SafePtr<GameplaySetup> GameplaySetup::instance;
    GameplaySetup* GameplaySetup::get_instance() {
        if (!instance) {
            instance.emplace(GameplaySetup::New_ctor());
            instance->reuseIdentifier = "GameplaySetupCell";
        }
        return instance.ptr();
    }

    HMUI::TableView::IDataSource* GameplaySetup::i_DataSource() {
        return reinterpret_cast<HMUI::TableView::IDataSource*>(this);
    }

    void GameplaySetup::Setup() {
        DEBUG("Setup");
        auto menus = get_menus();
        if (menus.size() == 0) return;
        gameplaySetupViewController = UnityEngine::Resources::FindObjectsOfTypeAll<GlobalNamespace::GameplaySetupViewController*>()->FirstOrDefault();
        auto vanillaItems = get_vanillaItems();
        vanillaItems->Clear();

        auto t = gameplaySetupViewController->get_transform();
        int childCount = t->get_childCount();
        for (int i = 0; i < childCount; i++) {
            auto child = t->GetChild(i);
            if (child->get_name() != "HeaderPanel") {
                vanillaItems->Add(child);
            }
        }

        auto textSegmentedControl = t->Find("TextSegmentedControl").cast<UnityEngine::RectTransform>();
        textSegmentedControl->set_sizeDelta({0, 6});
        layoutGroup = textSegmentedControl->GetComponent<UnityEngine::UI::LayoutGroup*>();

        parse_and_construct(Assets::GameplayMenu::GameplaySetup_bsml, t, this);

        modsList->tableView->SetDataSource(this->i_DataSource(), false);
        listParsed = false;

        auto didActivate = custom_types::MakeDelegate<HMUI::ViewController::DidActivateDelegate*>(
            std::function<void(bool, bool, bool)>(
            [this](bool firstActivation, bool addedToHierarchy, bool screenSystemEnabling) {
            this->GameplaySetupDidActivate(firstActivation, addedToHierarchy, screenSystemEnabling);
        }));
        gameplaySetupViewController->add_didActivateEvent(didActivate);

        auto didDeactivate = custom_types::MakeDelegate<HMUI::ViewController::DidDeactivateDelegate*>(
            std::function<void(bool, bool)>(
            [this](bool removedFromHierarchy, bool screenSystemDisabling) {
            this->GameplaySetupDidDeactivate(removedFromHierarchy, screenSystemDisabling);
        }));
        gameplaySetupViewController->add_didDeactivateEvent(didDeactivate);
        listModal->onHide = std::bind(&GameplaySetup::ClickedOffModal, this);
    }

    void GameplaySetup::GameplaySetupDidActivate(bool firstActivation, bool addedToHierarchy, bool screenSystemEnabling) {
        DEBUG("DidActivate");
        layoutGroup->m_RectChildren->Clear();

        MenuType menuType = MenuType::Custom;
        auto fc = Helpers::GetMainFlowCoordinator()->YoungestChildFlowCoordinatorOrSelf();
        if (fc.try_cast<GlobalNamespace::CampaignFlowCoordinator>().has_value()) {
            menuType = MenuType::Campaign;
        } else if (fc.try_cast<GlobalNamespace::SinglePlayerLevelSelectionFlowCoordinator>().has_value()) {
            menuType = MenuType::Solo;
        } else if (fc.try_cast<GlobalNamespace::GameServerLobbyFlowCoordinator>().has_value()) {
            menuType = MenuType::Online;
        }

        for (auto x : get_menus()) {
            auto menu = reinterpret_cast<GameplaySetupMenu*>(x);
            menu->SetVisible(menu->IsMenuType(menuType));
        }
    }

    void GameplaySetup::GameplaySetupDidDeactivate(bool removedFromHierarchy, bool screenSystemDisabling) {
        DEBUG("DidDeactivate(removedFromHierarchy: {}, screenSystemDisabling: {})", removedFromHierarchy, screenSystemDisabling);

        if (!screenSystemDisabling) {
            tabSelector->textSegmentedControl->SelectCellWithNumber(0);
            vanillaTab->get_gameObject()->SetActive(true);
            modsTab->get_gameObject()->SetActive(false);
        }
    }

    void GameplaySetup::ClickedOffModal() {
        GameplaySetupDidActivate(false, false, false);
    }

    void GameplaySetup::ShowModal() {
        DEBUG("ShowModal");
        set_loaded(false);
        auto delegate = MakeSystemAction([this]{
            DEBUG("Modal is shown");
            if (!this->listParsed) {
                DEBUG("Reloading data in list");
                this->modsList->tableView->ReloadData();
                this->listParsed = true;
            }
            DEBUG("Refreshing size");
            this->modsList->tableView->RefreshContentSize();
            set_loaded(true);
        });
        listModal->HMUI::ModalView::Show(true, true, delegate);
    }

    bool GameplaySetup::AddTab(std::string_view name, std::string_view content_key, System::Object* host, MenuType menuType) {
        auto menus = get_menus();
        auto menu = std::find_if(menus.begin(), menus.end(), [name](auto x){ return reinterpret_cast<GameplaySetupMenu*>(x)->name == name; });
        if (menu != menus.end()) {
            return false;
        }

        menus->Add(GameplaySetupMenu::Make_new(name, content_key, host, menuType));
        return true;
    }

    bool GameplaySetup::AddTab(System::Type* csType, std::string_view name, MenuType menuType) {
        auto menus = get_menus();
        auto menu = std::find_if(menus.begin(), menus.end(), [name](auto x){ return reinterpret_cast<GameplaySetupMenu*>(x)->name == name; });
        if (menu != menus.end()) {
            return false;
        }
        menus->Add(GameplaySetupMenu::Make_new(csType, name, menuType));
        return true;
    }

    bool GameplaySetup::AddTab(std::function<void(UnityEngine::GameObject*, bool)> didActivate, std::string_view name, MenuType menuType) {
        auto menus = get_menus();
        auto menu = std::find_if(menus.begin(), menus.end(), [name](auto x){ return reinterpret_cast<GameplaySetupMenu*>(x)->name == name; });
        if (menu != menus.end()) {
            return false;
        }
        menus->Add(GameplaySetupMenu::Make_new(didActivate, name, menuType));
        return true;
    }

    void GameplaySetup::SetTabVisibility(std::string_view name, bool isVisible) {
        if (!gameplaySetupViewController || !gameplaySetupViewController->m_CachedPtr.m_value || !gameplaySetupViewController->get_isActiveAndEnabled()) {
            return;
        }

        auto menus = get_menus();
        auto menu = std::find_if(menus.begin(), menus.end(), [name](auto x){ return reinterpret_cast<GameplaySetupMenu*>(x)->name == name; });
        if (menu != menus.end()) {
            reinterpret_cast<GameplaySetupMenu*>(*menu)->SetVisible(isVisible);
        }
    }

    bool GameplaySetup::RemoveTab(std::string_view name) {
        auto menus = get_menus();
        for (auto x : menus) {
            auto menu = reinterpret_cast<GameplaySetupMenu*>(x);
            if (menu->name == name) {
                menus->Remove(menu);
                return true;
            }
        }
        return false;
    }

    GameplaySetupCell* GameplaySetup::GetCell() {
        INFO("Getting Cell");
        auto cell = modsList->tableView->DequeueReusableCellForIdentifier(reuseIdentifier).try_cast<GameplaySetupCell>().value_or(nullptr);

        if (!cell || !cell->m_CachedPtr.m_value) {
            cell = UnityEngine::GameObject::New_ctor("GameplaySetupCell")->AddComponent<GameplaySetupCell*>();
            cell->set_interactable(true);
            cell->set_reuseIdentifier(reuseIdentifier);

            parse_and_construct(Assets::GameplayMenu::GameplaySetupCell_bsml, cell->get_transform(), cell);
        }

        return cell;
    }

    float GameplaySetup::CellSize() {
        return 8.0f;
    }

    int GameplaySetup::NumberOfCells() {
        return get_menus().size();
    }

    HMUI::TableCell* GameplaySetup::CellForIdx(HMUI::TableView* tableView, int idx) {
        return GetCell()->PopulateCell(reinterpret_cast<GameplaySetupMenu*>(get_menus()[idx]));
    }


    ListW<System::Object*> GameplaySetup::get_menus() {
        if (!_menus) {
            _menus = ListW<System::Object*>::New();
        }
        return _menus;
    }

    ListW<UnityEngine::Transform*> GameplaySetup::get_vanillaItems() {
        if (!_vanillaItems) {
            _vanillaItems = ListW<UnityEngine::Transform*>::New();
        }
        return _vanillaItems;
    }

    bool GameplaySetup::get_loaded() {
        return _loaded;
    }

    void GameplaySetup::set_loaded(bool value) {
        _loaded = value;
        if (modsList && modsList->m_CachedPtr.m_value) modsList->get_gameObject()->SetActive(value);
        if (loading && loading->m_CachedPtr.m_value) loading->get_gameObject()->SetActive(!value);
    }
}
