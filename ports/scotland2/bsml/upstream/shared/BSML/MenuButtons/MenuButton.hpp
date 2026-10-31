#pragma once

#include "custom-types/shared/macros.hpp"
#include "../../macros.hpp"
#include "UnityEngine/UI/Button.hpp"
#include <functional>

DECLARE_CLASS_CODEGEN(BSML, MenuButton, System::Object) {
    DECLARE_INSTANCE_FIELD(StringW, text);
    DECLARE_INSTANCE_FIELD(StringW, hoverHint);
    DECLARE_INSTANCE_FIELD(UnityEngine::UI::Button*, menuButton);

    DECLARE_BSML_PROPERTY(bool, interactable);
    DECLARE_INSTANCE_METHOD(void, Click);

    DECLARE_DEFAULT_CTOR();

    public:
        static MenuButton* Make_new(std::string_view label, std::string_view hoverHint = "", std::function<void()> onClick = nullptr);

    private:
        std::function<void(void)> onClick;
};