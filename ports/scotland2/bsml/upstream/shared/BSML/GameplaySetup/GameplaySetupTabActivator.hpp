#pragma once

#include "custom-types/shared/macros.hpp"
#include "UnityEngine/MonoBehaviour.hpp"
#include "../MenuSource.hpp"

DECLARE_CLASS_CODEGEN(BSML, GameplaySetupTabActivator, UnityEngine::MonoBehaviour) {
    DECLARE_INSTANCE_FIELD(MenuSource, menuSource);
    DECLARE_INSTANCE_FIELD(UnityEngine::MonoBehaviour*, mb);
    DECLARE_INSTANCE_FIELD(bool, _activatedBefore);

    DECLARE_INSTANCE_METHOD(void, OnEnable);
    DECLARE_INSTANCE_METHOD(void, OnDisable);
    public:
        std::function<void(UnityEngine::GameObject*, bool)> didActivate;
};