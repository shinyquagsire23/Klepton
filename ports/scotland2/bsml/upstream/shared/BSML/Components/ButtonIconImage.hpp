#pragma once

#include "custom-types/shared/macros.hpp"
#include "UnityEngine/MonoBehaviour.hpp"
#include "UnityEngine/UI/Image.hpp"
#include "UnityEngine/Sprite.hpp"

DECLARE_CLASS_CODEGEN(BSML, ButtonIconImage, UnityEngine::MonoBehaviour) {
    DECLARE_INSTANCE_FIELD(UnityEngine::UI::Image*, image);

    public:
        void SetIcon(StringW path);
        void SetIcon(UnityEngine::Sprite* sprite);
};