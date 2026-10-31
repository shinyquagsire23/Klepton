#include "BSML.hpp"
#include "logging.hpp"
#include "BSML/MenuButtons/MenuButtons.hpp"
#include "BSML/Settings/BSMLSettings.hpp"
#include "BSML/GameplaySetup/GameplaySetup.hpp"
#include "BSML/FlowCoordinators/MainMenuHolderFlowCoordinator.hpp"

extern "C" void load();
extern "C" void late_load();

namespace BSML {
    void Init() {
        load();
    }

    void LateInit() {
        load();
        late_load();
    }

    std::shared_ptr<BSMLParser> parse(std::string_view str) {
        Init();
        return BSMLParser::parse(str);
    }

    std::shared_ptr<BSMLParser> parse_and_construct(std::string_view str, UnityEngine::Transform* parent, System::Object* host) {
        Init();
        return BSMLParser::parse_and_construct(str, parent, host);
    }

    namespace Register {
        MenuButton* RegisterMenuButton(std::string_view text, std::string_view hoverHint, std::function<void(void)> onClick) {
            Init();
            auto btn = MenuButton::Make_new(text, hoverHint, onClick);
            if (RegisterMenuButton(btn)) return btn;
            return nullptr;
        }

        bool RegisterMenuButton(MenuButton* button) {
            Init();
            return MenuButtons::get_instance()->Registerbutton(button);
        }

        bool UnRegisterMenuButton(MenuButton* button) {
            Init();
            return MenuButtons::get_instance()->UnRegisterbutton(button);
        }

        bool RegisterSettingsMenu(std::string_view name, std::string_view content_key, System::Object* host, bool enableExtraButtons) {
            Init();
            return BSMLSettings::get_instance()->TryAddSettingsMenu(name, content_key, host, enableExtraButtons);
        }

        bool RegisterSettingsMenu(std::string_view name, System::Type* csType, MenuSource menuSource, bool enableExtraButtons) {
            Init();
            if (menuSource != BSML::MenuSource::ViewController && menuSource != BSML::MenuSource::FlowCoordinator)
                throw std::runtime_error("Menu Source was not view controller or flow coordinator!");
            return BSMLSettings::get_instance()->TryAddSettingsMenu(csType, name, menuSource, enableExtraButtons);
        }

        bool RegisterSettingsMenu(std::string_view name, std::function<void(HMUI::ViewController*, bool, bool, bool)> viewControllerDidActivate, bool enableExtraButtons) {
            Init();
            return BSMLSettings::get_instance()->TryAddSettingsMenu(viewControllerDidActivate, name, enableExtraButtons);
        }

        bool UnRegisterSettingsMenu(System::Object* host) {
            Init();
            return BSMLSettings::get_instance()->RemoveSettingsMenu(host);
        }

        bool RegisterGameplaySetupTab(std::string_view name, std::string_view content_key, System::Object* host, MenuType menuType) {
            Init();
            return BSML::GameplaySetup::get_instance()->AddTab(name, content_key, host, menuType);
        }

        bool RegisterGameplaySetupTab(System::Type* csType, std::string_view name, MenuType menuType) {
            Init();
            return BSML::GameplaySetup::get_instance()->AddTab(csType, name, menuType);
        }

        bool RegisterGameplaySetupTab(std::string_view name, std::function<void(UnityEngine::GameObject*, bool)> didActivate, MenuType menuType) {
            Init();
            return BSML::GameplaySetup::get_instance()->AddTab(didActivate, name, menuType);
        }

        bool UnRegisterGameplaySetupTab(std::string_view name) {
            Init();
            return BSML::GameplaySetup::get_instance()->RemoveTab(name);
        }

        void RegisterMainMenuFlowCoordinator(const std::string_view& buttonText, const std::string_view& hoverhint, System::Type* flowCoordinatorType) {
            Init();
            AddMainMenuRegistration(new MainMenuRegistration("", buttonText, hoverhint, flowCoordinatorType, MenuSource::FlowCoordinator));
        }

        void RegisterMainMenuViewController(const std::string_view& title, const std::string_view& buttonText, const std::string_view& hoverhint, System::Type* viewControllerType) {
            Init();
            AddMainMenuRegistration(new MainMenuRegistration(title, buttonText, hoverhint, viewControllerType, MenuSource::ViewController));
        }

        void RegisterMainMenuViewControllerMethod(const std::string_view& title, const std::string_view& buttonText, const std::string_view& hoverhint, std::function<void(HMUI::ViewController*, bool, bool, bool)> viewControllerDidActivate) {
            Init();
            AddMainMenuRegistration(new MainMenuRegistration(title, buttonText, hoverhint, viewControllerDidActivate));
        }

        void AddMainMenuRegistration(MainMenuRegistration* reg) {
            Init();
            DEBUG("Registering {}", reg->buttonText);

            MainMenuRegistration::registrations.emplace_back(reg);
            RegisterMenuButton(reg->buttonText, reg->hoverHint, std::bind(&MainMenuRegistration::Present, reg));
        }
    }

    namespace Events {
        UnorderedEventCallback<> onGameDidRestart;
    }
}
