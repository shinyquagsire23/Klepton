#pragma once

#include "custom-types/shared/macros.hpp"
#include "HMUI/ModalView.hpp"

DECLARE_CLASS_CODEGEN(BSML, ModalView, HMUI::ModalView) {
    DECLARE_INSTANCE_FIELD(bool, moveToCenter);
    DECLARE_INSTANCE_FIELD(bool, dismissOnBlockerClicked);
    DECLARE_INSTANCE_METHOD(void, Awake);

    public:
    DECLARE_INSTANCE_METHOD(void, Show);
    DECLARE_INSTANCE_METHOD(void, Hide);
    DECLARE_INSTANCE_METHOD(void, BlockerClicked);

    public:
        std::function<void(void)> onHide;
};