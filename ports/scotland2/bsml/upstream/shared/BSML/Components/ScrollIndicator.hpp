#pragma once

#include "custom-types/shared/macros.hpp"
#include "UnityEngine/RectTransform.hpp"
#include "HMUI/VerticalScrollIndicator.hpp"

DECLARE_CLASS_CODEGEN(BSML, ScrollIndicator, HMUI::VerticalScrollIndicator) {
    DECLARE_INSTANCE_METHOD(UnityEngine::RectTransform *, get_Handle);
    DECLARE_INSTANCE_METHOD(void, set_Handle, UnityEngine::RectTransform *value);

};