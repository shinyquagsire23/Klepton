#pragma once

#include "custom-types/shared/macros.hpp"
#include "../Components/Tab.hpp"
#include "MenuType.hpp"
#include "../MenuSource.hpp"
#include <string>

DECLARE_CLASS_CODEGEN(BSML, GameplaySetupMenu, System::Object) {
    DECLARE_INSTANCE_FIELD(StringW, name);
    DECLARE_INSTANCE_FIELD(StringW, content_key);
    DECLARE_INSTANCE_FIELD(System::Object*, host);
    DECLARE_INSTANCE_FIELD(MenuType, menuType);
    DECLARE_INSTANCE_FIELD(Tab*, tab);
    DECLARE_INSTANCE_FIELD(MenuSource, menuSource);
    DECLARE_INSTANCE_FIELD(System::Type*, csType);

    DECLARE_INSTANCE_METHOD(bool, get_visible);
    DECLARE_INSTANCE_METHOD(void, set_visible, bool value);

    DECLARE_INSTANCE_METHOD(void, Setup);
    DECLARE_INSTANCE_METHOD(void, PostParse);
    DECLARE_INSTANCE_METHOD(void, SetVisible, bool isVisible);

    public:
        static GameplaySetupMenu* Make_new(std::string_view name, std::string_view content_key, System::Object* host, BSML::MenuType menuType = BSML::MenuType::All);
        static GameplaySetupMenu* Make_new(System::Type* csType, std::string_view name, BSML::MenuType menuType = BSML::MenuType::All);
        static GameplaySetupMenu* Make_new(std::function<void(UnityEngine::GameObject*, bool)> didActivate, std::string_view name, BSML::MenuType menuType = BSML::MenuType::All);

        bool get_content(std::string_view& content);
        bool IsMenuType(BSML::MenuType toCheck);

        DECLARE_DEFAULT_CTOR();
        std::function<void(UnityEngine::GameObject*, bool)> didActivate;
};