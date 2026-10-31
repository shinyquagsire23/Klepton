#pragma once

#include "../_config.h"
#include "UnityEngine/Material.hpp"
#include "VRUIControls/PhysicsRaycasterWithCache.hpp"
#include "Zenject/DiContainer.hpp"
#include "HMUI/HoverHintController.hpp"
#include "TMPro/TMP_FontAsset.hpp"
#include "GlobalNamespace/IVRPlatformHelper.hpp"
#include "GlobalNamespace/MainFlowCoordinator.hpp"

namespace BSML::Helpers {
    BSML_EXPORT VRUIControls::PhysicsRaycasterWithCache* GetPhysicsRaycasterWithCache();
    BSML_EXPORT Zenject::DiContainer* GetDiContainer();
    BSML_EXPORT HMUI::HoverHintController* GetHoverHintController();

    BSML_EXPORT TMPro::TMP_FontAsset* GetMainTextFont();
    BSML_EXPORT UnityEngine::Material* GetMainUIFontMaterial();
    BSML_EXPORT UnityEngine::Material* GetUINoGlowMat();
    BSML_EXPORT GlobalNamespace::IVRPlatformHelper* GetIVRPlatformHelper();
    BSML_EXPORT GlobalNamespace::MainFlowCoordinator* GetMainFlowCoordinator();
}
