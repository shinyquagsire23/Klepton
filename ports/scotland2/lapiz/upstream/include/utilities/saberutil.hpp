#pragma once

#include "GlobalNamespace/SaberModelController.hpp"
#include "GlobalNamespace/SaberTrail.hpp"
#include "GlobalNamespace/TubeBloomPrePassLight.hpp"
#include "GlobalNamespace/SetSaberGlowColor.hpp"
#include "GlobalNamespace/SetSaberFakeGlowColor.hpp"
#include "GlobalNamespace/Parametric3SliceSpriteController.hpp"
#include "UnityEngine/Color.hpp"
#include "UnityEngine/MeshRenderer.hpp"
#include "UnityEngine/MaterialPropertyBlock.hpp"

namespace SaberUtil {
    static inline UnityEngine::Color operator*(UnityEngine::Color a, UnityEngine::Color b) {
        return {
            a.r * b.r,
            a.g * b.g,
            a.b * b.b,
            a.a * b.a
        };
    }

    static UnityEngine::Color GetColor(GlobalNamespace::SaberModelController* saberModelController) {
        return saberModelController->_saberTrail->_color;
    }

    static UnityEngine::Color ColorWithAlpha(UnityEngine::Color color, float alpha) {
        color.a = alpha;
        return color;
    }

    static void SetColors(GlobalNamespace::SetSaberFakeGlowColor* setSaberFakeGlowColor, UnityEngine::Color color) {
            auto parametricSpriteController = setSaberFakeGlowColor->_parametric3SliceSprite;
            auto tint = setSaberFakeGlowColor->_tintColor;
            parametricSpriteController->color = color * tint;
            parametricSpriteController->Refresh();
    }

    static void SetColors(GlobalNamespace::SetSaberGlowColor* setSaberGlowColor, UnityEngine::Color color) {
            auto meshRenderer = setSaberGlowColor->_meshRenderer;
            auto materialPropertyBlock = setSaberGlowColor->_materialPropertyBlock;
            auto propertyTintPairs = setSaberGlowColor->_propertyTintColorPairs;

            if (!materialPropertyBlock || !materialPropertyBlock->m_Ptr.m_value) {
                materialPropertyBlock = UnityEngine::MaterialPropertyBlock::New_ctor();
                setSaberGlowColor->_materialPropertyBlock = materialPropertyBlock;
            }

            for (auto tintPair : propertyTintPairs)
                materialPropertyBlock->SetColor(tintPair->property, color * tintPair->tintColor);

            meshRenderer->SetPropertyBlock(materialPropertyBlock);
    }

    static void SetColor(GlobalNamespace::SaberModelController* saberModelController, UnityEngine::Color color) {
        auto trail = saberModelController->_saberTrail;

        auto light = saberModelController->_saberLight;
        auto glowColors = saberModelController->_setSaberGlowColors;
        auto fakeGlowColors = saberModelController->_setSaberFakeGlowColors;

        float beforeAlpha = trail->_color.a;
        trail->_color = trail->_colorOverwrite ? trail->_forcedColor : ColorWithAlpha(color, beforeAlpha);
        for (auto glow : glowColors)
            SetColors(glow, color);
        for (auto fakeGlow : fakeGlowColors)
            SetColors(fakeGlow, color);
        if (light && light->m_CachedPtr.m_value)
            light->set_color(color);
    }
}
