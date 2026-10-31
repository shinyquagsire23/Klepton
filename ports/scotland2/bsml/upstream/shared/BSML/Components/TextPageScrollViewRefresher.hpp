#pragma once

#include "custom-types/shared/macros.hpp"

#include "UnityEngine/MonoBehaviour.hpp"
#include "HMUI/TextPageScrollView.hpp"

DECLARE_CLASS_CODEGEN(BSML, TextPageScrollViewRefresher, UnityEngine::MonoBehaviour) {
    DECLARE_INSTANCE_FIELD(HMUI::TextPageScrollView*, scrollView);
    DECLARE_INSTANCE_METHOD(void, OnEnable);
    DECLARE_INSTANCE_METHOD(void, OnRectTransformDimensionsChange);
};