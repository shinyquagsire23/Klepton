#include "BSML/Settings/SettingsMenu.hpp"
#include "logging.hpp"

#include "Helpers/creation.hpp"
#include "BSML.hpp"
#include "BSMLDataCache_internal.hpp"
#include "assets.hpp"

#include "UnityEngine/RectTransform.hpp"
#include "UnityEngine/Vector2.hpp"
#include "custom-types/shared/delegate.hpp"

DEFINE_TYPE(BSML, SettingsMenu);

namespace BSML {
    void SettingsMenu::Finalize() {
        // clear parser params
        parserParams = nullptr;

        auto objectFinalize = il2cpp_utils::il2cpp_type_check::MetadataGetter<&System::Object::Finalize>::methodInfo();
        il2cpp_utils::RunMethodRethrow<void, false>(this, objectFinalize);

        this->~SettingsMenu();
    }

    bool SettingsMenu::get_didSetup() {
        return viewController && viewController->m_CachedPtr.m_value || flowCoordinator && flowCoordinator->m_CachedPtr.m_value;
    }

    HMUI::ViewController* SettingsMenu::get_viewController() {
        if (!get_didSetup()) {
            Setup();
        }
        return viewController;
    }

    void SettingsMenu::Setup() {
        DEBUG("Setup");
        switch (menuSource) {
            case MenuSource::BSMLContent:
                viewController = Helpers::CreateViewController<HMUI::ViewController*>();
                SetupViewControllerTransform(viewController);
                parserParams = parse_and_construct(get_content(), viewController->get_transform(), host)->parserParams;
                break;
            case MenuSource::FlowCoordinator:
                flowCoordinator = Helpers::CreateFlowCoordinator(csType);
                break;
            case MenuSource::ViewController:
                viewController = Helpers::CreateViewController(csType);
                SetupViewControllerTransform(viewController);
                break;
            case MenuSource::Method:
                viewController = Helpers::CreateViewController<HMUI::ViewController*>();
                SetupViewControllerTransform(viewController);
                viewController->add_didActivateEvent(custom_types::MakeDelegate<HMUI::ViewController::DidActivateDelegate*>(viewController, viewControllerDidActivate));
                break;
            case MenuSource::Component:
                throw std::runtime_error("Invalid menusource passed to settings menu!");
        }
    }

    std::string_view SettingsMenu::get_content() {
        auto entry = DataCache::Get(content_key);
        if (!entry) {
            return Assets::Settings::Error_bsml;
        }

        auto data = entry->get_data();
        return std::string_view(reinterpret_cast<char*>(data->begin()), size_t(data.size()));
    }

    void SettingsMenu::SetupViewControllerTransform(HMUI::ViewController* viewController) {
        auto r = viewController->get_rectTransform();
        r->set_sizeDelta({110, 0});
        r->set_anchorMin({0.5f, 0});
        r->set_anchorMax({0.5f, 1});
    }

    SettingsMenu* SettingsMenu::Make_new(std::string_view name, std::string_view content, System::Object* host, bool enableExtraButtons) {
        auto self = SettingsMenu::New_ctor();

        self->text = name;
        self->name = name;
        self->enableExtraButtons = enableExtraButtons;
        self->content_key = content;
        self->host = host;
        self->menuSource = MenuSource::BSMLContent;

        return self;
    }

    SettingsMenu* SettingsMenu::Make_new(std::string_view name, System::Type* csType, MenuSource menuSource, bool enableExtraButtons) {
        auto self = SettingsMenu::New_ctor();

        self->text = name;
        self->name = name;
        self->enableExtraButtons = enableExtraButtons;
        self->csType = csType;
        self->menuSource = menuSource;

        return self;
    }

    SettingsMenu* SettingsMenu::Make_new(std::string_view name, std::function<void(HMUI::ViewController*, bool, bool, bool)> viewControllerDidActivate, bool enableExtraButtons) {
        auto self = SettingsMenu::New_ctor();

        self->text = name;
        self->name = name;
        self->enableExtraButtons = enableExtraButtons;
        self->viewControllerDidActivate = viewControllerDidActivate;
        self->menuSource = MenuSource::Method;

        return self;
    }

}
