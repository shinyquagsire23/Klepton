#include "BSML/GameplaySetup/GameplaySetupMenu.hpp"
#include "BSML/GameplaySetup/GameplaySetupTabActivator.hpp"
#include "config.hpp"
#include "logging.hpp"

#include "assets.hpp"
#include "BSML.hpp"
#include "BSMLDataCache_internal.hpp"

DEFINE_TYPE(BSML, GameplaySetupMenu);

namespace BSML {
    GameplaySetupMenu* GameplaySetupMenu::Make_new(std::string_view name, std::string_view content_key, System::Object* host, MenuType menuType) {
        auto self = GameplaySetupMenu::New_ctor();

        self->name = name;
        self->content_key = content_key;
        self->host = host;
        self->menuType = menuType;
        self->menuSource = MenuSource::BSMLContent;

        return self;
    }

    GameplaySetupMenu* GameplaySetupMenu::Make_new(System::Type* csType, std::string_view name, BSML::MenuType menuType) {
        auto self = GameplaySetupMenu::New_ctor();

        self->name = name;
        self->menuType = menuType;
        self->csType = csType;
        self->menuSource = MenuSource::Component;

        return self;
    }

    GameplaySetupMenu* GameplaySetupMenu::Make_new(std::function<void(UnityEngine::GameObject*, bool)> didActivate, std::string_view name, BSML::MenuType menuType) {
        auto self = GameplaySetupMenu::New_ctor();

        self->name = name;
        self->menuType = menuType;
        self->didActivate = didActivate;
        self->menuSource = MenuSource::Method;

        return self;
    }


    void GameplaySetupMenu::Setup() {
        switch (menuSource) {
            case MenuSource::BSMLContent: {
                std::string_view content;
                if (get_content(content)) {
                    DEBUG("Got content! parsing...");
                    parse_and_construct(content, tab->get_transform(), host);
                } else { // there was an error
                    DEBUG("Error getting content, was the content key correctly given?");
                    parse_and_construct(content, tab->get_transform(), tab);
                }
                break;
            }
            case MenuSource::Method: {
                    auto activator = tab->get_gameObject()->AddComponent<GameplaySetupTabActivator*>();
                    activator->didActivate = didActivate;
                    activator->menuSource = menuSource;
                }
                break;
            case MenuSource::Component: {
                    auto activator = tab->gameObject->AddComponent<GameplaySetupTabActivator*>();
                    activator->mb = UnityW<UnityEngine::MonoBehaviour>(tab->gameObject->AddComponent(csType));
                    activator->menuSource = menuSource;
                }
                break;
            case MenuSource::ViewController: [[fallthrough]];
            case MenuSource::FlowCoordinator:
                throw std::runtime_error("Invalid menu source passed to gameplay setup!");
        }
    }

    void GameplaySetupMenu::PostParse() {
        Setup();
    }

    void GameplaySetupMenu::SetVisible(bool isVisible) {
        if (tab && tab->m_CachedPtr.m_value) tab->set_isVisible(isVisible && get_visible());
    }

    bool GameplaySetupMenu::IsMenuType(MenuType toCheck) {
        return (menuType & toCheck) == toCheck;
    }

    bool GameplaySetupMenu::get_content(std::string_view& content) {
        auto entry = DataCache::Get(content_key);
        if (!entry) {
            content = Assets::GameplayMenu::GameplayTabError_bsml;
            return false;
        }
        auto data = entry->get_data();
        content = std::string_view((const char*)data->begin(), data.size());
        return true;
    }

    bool GameplaySetupMenu::get_visible() {
        return std::find(config.hiddenTabs.begin(), config.hiddenTabs.end(), name) == config.hiddenTabs.end();
    }

    void GameplaySetupMenu::set_visible(bool value) {
        if (value) {
            auto itr = std::find(config.hiddenTabs.begin(), config.hiddenTabs.end(), name);
            if (itr != config.hiddenTabs.end()) config.hiddenTabs.erase(itr);
        }
        else if (get_visible()) {
            config.hiddenTabs.push_back(name);
        }
        SaveConfig();
    }
}
