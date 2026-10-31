#include "BSML/Settings/UI/ModSettingsFlowCoordinator.hpp"
#include "BSML/Settings/BSMLSettings.hpp"
#include "logging.hpp"

#include "assets.hpp"
#include "BSML.hpp"
#include "Helpers/creation.hpp"
#include "Helpers/getters.hpp"

#include "UnityEngine/Resources.hpp"
#include "GlobalNamespace/MenuTransitionsHelper.hpp"

DEFINE_TYPE(BSML, ModSettingsFlowCoordinator);

namespace BSML {
    void ModSettingsFlowCoordinator::ctor() {
        static auto baseKlass = classof(HMUI::FlowCoordinator*);
        custom_types::InvokeBaseCtor(baseKlass, this);
        submenuStack = Stack<HMUI::ViewController*>::New_ctor();
    }

    void ModSettingsFlowCoordinator::DidActivate(bool firstActivation, bool addedToHierarchy, bool screenSystemEnabling) {
        if (!firstActivation) return;
        DEBUG("DidActivate");
        SetTitle("Mod Settings", HMUI::ViewController::AnimationType::In);
        set_showBackButton(true);
        navigationController = Helpers::CreateViewController<HMUI::NavigationController*>();
        auto parser = parse_and_construct(Assets::Settings::Buttons_bsml, navigationController->get_transform(), this);
        parserParams = parser->parserParams;

        settingsMenuListViewController = Helpers::CreateViewController<SettingsMenuListViewController*>();
        settingsMenuListViewController->clickedMenu = [this](auto menu) { this->OpenMenu(menu); };

        SetViewControllerToNavigationController(navigationController, settingsMenuListViewController);
        ProvideInitialViewControllers(navigationController, nullptr, nullptr, nullptr, nullptr);
    }

    void ModSettingsFlowCoordinator::BackButtonWasPressed(HMUI::ViewController* topViewController) {
        Cancel();
    }

    void ModSettingsFlowCoordinator::ShowInitial() {
        DEBUG("Showing Initial");
        if (activeController && activeController->m_CachedPtr.m_value) {
            return;
        }

        settingsMenuListViewController->list->tableView->SelectCellWithIdx(0, false);
        auto menus = BSMLSettings::get_instance()->get_settingsMenus();
        if (menus.size() > 0) {
            OpenMenu(reinterpret_cast<SettingsMenu*>(menus[0]));
        }
        isPresenting = true;
    }

    void ModSettingsFlowCoordinator::OpenMenu(SettingsMenu* menu) {
        if (!menu->get_didSetup()) {
            menu->Setup();
            if (menu->menuSource == MenuSource::BSMLContent)
                menu->parserParams->AddEvent("back", std::bind(&ModSettingsFlowCoordinator::Back, this));
        }

        if (bottomButtons && bottomButtons->m_CachedPtr.m_value) {
            auto btns = bottomButtons->GetComponentsInChildren<UnityEngine::UI::Button*>();
            for (auto btn : btns) {
                btn->set_interactable(menu->enableExtraButtons);
            }
        }

        if (menu->menuSource == MenuSource::FlowCoordinator) {
            settingsMenuListViewController->list->tableView->SelectCellWithIdx(0, true);
            PresentFlowCoordinator(menu->flowCoordinator, nullptr, HMUI::ViewController::AnimationDirection::Horizontal, false, false);
        } else {
            OpenMenu(menu->get_viewController(), false, false);
        }
    }

    void ModSettingsFlowCoordinator::OpenMenu(HMUI::ViewController* viewController, bool isSubmenu, bool isBack) {
        if (isPresenting) return;
        if (!isBack) {
            if (isSubmenu)
                submenuStack->Push(activeController);
            else
                submenuStack->Clear();
        }

        bool wasActive = activeController && activeController->m_CachedPtr.m_value;
        if (wasActive)
            PopViewControllerFromNavigationController(navigationController, nullptr, true);
        auto delegate = MakeSystemAction([&isPresenting = this->isPresenting, bottomButtons = this->bottomButtons, navigationController = this->navigationController]{
            isPresenting = false;
            if (bottomButtons && bottomButtons->m_CachedPtr.m_value) {
                bottomButtons->SetAsLastSibling();
            }
        });
        PushViewControllerToNavigationController(navigationController, viewController, delegate, wasActive);
        activeController = viewController;
    }

    void ModSettingsFlowCoordinator::Ok() {
        EmitEventToAll("apply");
        // make ur mods not crash on soft restart lul
        UnityEngine::Resources::FindObjectsOfTypeAll<GlobalNamespace::MenuTransitionsHelper*>()->First()->RestartGame(nullptr);
    }

    void ModSettingsFlowCoordinator::Cancel() {
        if (isPresenting || isAnimating) return;
        _parentFlowCoordinator->DismissFlowCoordinator(this, HMUI::ViewController::AnimationDirection::Vertical, nullptr, false);
        EmitEventToAll("cancel");
    }

    void ModSettingsFlowCoordinator::Back() {
        if (submenuStack->get_Count() > 0) {
            OpenMenu(submenuStack->Pop(), false, true);
        }
    }

    void ModSettingsFlowCoordinator::EmitEventToAll(std::string_view ev) {
        auto menus = BSMLSettings::get_instance()->get_settingsMenus();
        for (auto cell : menus) {
            auto menu = reinterpret_cast<SettingsMenu*>(cell);
            if (menu->get_didSetup() && menu->parserParams) {
                menu->parserParams->EmitEvent(std::string(ev));
            }
        }

    }
}
