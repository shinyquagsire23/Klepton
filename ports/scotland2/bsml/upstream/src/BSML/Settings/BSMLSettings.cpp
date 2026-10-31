#include "BSML/Settings/BSMLSettings.hpp"
#include "BSML/Settings/SettingsMenu.hpp"
#include "logging.hpp"

#include "Helpers/creation.hpp"
#include "Helpers/utilities.hpp"
#include "Helpers/getters.hpp"
#include "Helpers/delegates.hpp"

#include "assets.hpp"

#include "UnityEngine/Object.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/WaitForFixedUpdate.hpp"
#include "UnityEngine/Resources.hpp"
#include "UnityEngine/TextureWrapMode.hpp"
#include "BGLib/Polyglot/LocalizedTextMeshProUGUI.hpp"

#include "BSML/SharedCoroutineStarter.hpp"
#include "GlobalNamespace/OptionsViewController.hpp"
#include "HMUI/ButtonSpriteSwap.hpp"

DEFINE_TYPE(BSML, BSMLSettings);

namespace BSML {
    SafePtr<BSMLSettings> BSMLSettings::instance;
    BSMLSettings* BSMLSettings::get_instance() {
        if (!instance) {
            instance.emplace(BSMLSettings::New_ctor());
        }
        return instance.ptr();
    }

    ListW<BSML::CustomCellInfo*> BSMLSettings::get_settingsMenus() {
        if (!settingsMenus) {
            settingsMenus = ListW<CustomCellInfo*>::New();
        }
        return settingsMenus;
    }
    ModSettingsFlowCoordinator* BSMLSettings::get_modSettingsFlowCoordinator() {
        if (!modSettingsFlowCoordinator || !modSettingsFlowCoordinator->m_CachedPtr.m_value) {
            modSettingsFlowCoordinator = Helpers::CreateFlowCoordinator<ModSettingsFlowCoordinator*>();
        }
        return modSettingsFlowCoordinator;
    }

    void BSMLSettings::Setup() {
        DEBUG("Setup");
        auto menus = get_settingsMenus();
        for (auto& cell : menus) {
            auto menu = reinterpret_cast<BSML::SettingsMenu*>(cell);
            if (menu->viewController && menu->viewController->m_CachedPtr.m_value) {
                UnityEngine::Object::DestroyImmediate(menu->viewController->get_gameObject());
            }
        }

        auto starter = BSML::SharedCoroutineStarter::get_instance();
        if (addButtonCoroutine && addButtonCoroutine->m_Ptr.m_value) {
            starter->StopCoroutine(addButtonCoroutine);
        }

        if (!button || !button->m_CachedPtr.m_value) {
            addButtonCoroutine = starter->StartCoroutine(custom_types::Helpers::CoroutineHelper::New(AddButtonToMainScreen()));
        }

        isInitialized = true;
    }

    void BSMLSettings::TryAddBSMLMenu() {
        auto menus = get_settingsMenus();
        if (menus.size() == 0) {
            // add bsml about
            menus->Add(SettingsMenu::Make_new("BSML", MOD_ID "_settings_about", this, false));
        }
    }

    bool BSMLSettings::TryAddSettingsMenu(SettingsMenu* menu) {
        auto menus = get_settingsMenus();
        // if we find the same menu, return false
        if (std::find_if(menus.begin(), menus.end(), [&menu](auto x){ return x->text == menu->name;}) != menus.end()) {
            return false;
        }

        // this one didn't exist yet, so check if we should add the bsml menu (first)
        TryAddBSMLMenu();

        bool menuAdded = false;

        // Starting after the first entry, insert the new menu alphabetically
        for (int i = 1; i < menus.size(); i++) {
            auto existing = menus[i];
            if (menu->name < existing->text) {
                menus->Insert(i, menu);
                menuAdded = true;
                break;
            }
        }

        // Add the menu to the end, we didn't insert it anywhere
        if (!menuAdded) {
            menus->Add(menu);
        }

        if (isInitialized) {
            // per definition this is a new menu, so we can run setup if we're already initialized
            menu->Setup();
        }

        if (button && button->m_CachedPtr.m_value) {
            button->get_gameObject()->SetActive(true);
        }
        return true;
    }

    bool BSMLSettings::TryAddSettingsMenu(std::string_view name, std::string_view content_key, System::Object* host, bool enableExtraButtons) {
        auto menu = SettingsMenu::Make_new(name, content_key, host, enableExtraButtons);
        if (!TryAddSettingsMenu(menu)) {
            return false;
        }
        return true;
    }

    bool BSMLSettings::TryAddSettingsMenu(System::Type* csType, std::string_view name, MenuSource menuType, bool showExtraButtons) {
        auto menu = SettingsMenu::Make_new(name, csType, menuType, showExtraButtons);
        if (!TryAddSettingsMenu(menu)) {
            return false;
        }
        return true;
    }

    bool BSMLSettings::TryAddSettingsMenu(std::function<void(HMUI::ViewController*, bool, bool, bool)> viewControllerDidActivate, std::string_view name, bool showExtraButtons) {
        auto menu = SettingsMenu::Make_new(name, viewControllerDidActivate, showExtraButtons);
        if (!TryAddSettingsMenu(menu)) {
            return false;
        }
        return true;
    }

    bool BSMLSettings::RemoveSettingsMenu(System::Object* host) {
        auto menus = get_settingsMenus();
        for (auto& cell : menus) {
            auto menu = reinterpret_cast<BSML::SettingsMenu*>(cell);
            if (menu->host == host) {
                menus->Remove(menu);
                return true;
            }
        }
        return false;
    }

    void BSMLSettings::PresentSettings() {
        DEBUG("Presenting settings");
        auto modFC = get_modSettingsFlowCoordinator();
        modFC->isAnimating = true;

        auto fc = Helpers::GetMainFlowCoordinator()->YoungestChildFlowCoordinatorOrSelf();
        fc->PresentFlowCoordinator(modFC, MakeSystemAction([modFC]{
            modFC->ShowInitial();
            modFC->isAnimating = false;
        }), HMUI::ViewController::AnimationDirection::Horizontal, false, false);
    }

    custom_types::Helpers::Coroutine BSMLSettings::AddButtonToMainScreen() {
        addButtonCoroutine = nullptr;
        GlobalNamespace::OptionsViewController* optionsViewController = nullptr;
        auto wait = UnityEngine::WaitForFixedUpdate::New_ctor();
        while (!optionsViewController) {
            optionsViewController = UnityEngine::Resources::FindObjectsOfTypeAll<GlobalNamespace::OptionsViewController*>()->FirstOrDefault();
            co_yield reinterpret_cast<System::Collections::IEnumerator*>(wait);
        }

        button = UnityEngine::Object::Instantiate(optionsViewController->_settingsButton, optionsViewController->get_transform()->Find("Wrapper"));
        button->GetComponentInChildren<BGLib::Polyglot::LocalizedTextMeshProUGUI*>()->set_Key("Mod Settings");
        auto onClick = UnityEngine::UI::Button::ButtonClickedEvent::New_ctor();
        button->set_onClick(onClick);
        onClick->AddListener(MakeUnityAction(this, ___PresentSettings_MethodRegistrator.get()));

        if (get_settingsMenus().size() == 0)
            button->get_gameObject()->SetActive(false);

        normal = Utilities::LoadSpriteRaw(Assets::mods_idle_png);
        normal->get_texture()->set_wrapMode(UnityEngine::TextureWrapMode::Clamp);
        hover = Utilities::LoadSpriteRaw(Assets::mods_selected_png);
        hover->get_texture()->set_wrapMode(UnityEngine::TextureWrapMode::Clamp);

        auto buttonSpriteSwap = button->GetComponent<HMUI::ButtonSpriteSwap*>();
        buttonSpriteSwap->_disabledStateSprite = normal;
        buttonSpriteSwap->_normalStateSprite = normal;
        buttonSpriteSwap->_highlightStateSprite = hover;
        buttonSpriteSwap->_pressedStateSprite = hover;
        button->get_transform()->SetAsFirstSibling();
        co_return;
    }
}
