#include "BSML/Components/Backgroundable.hpp"
#include "logging.hpp"
#include <map>

#include "UnityEngine/Resources.hpp"
#include "UnityEngine/Sprite.hpp"
#include "UnityEngine/Transform.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/HideFlags.hpp"
#include "UnityEngine/Color.hpp"

#include "System/Collections/Generic/Dictionary_2.hpp"

DEFINE_TYPE(BSML, Backgroundable);

std::map<std::string, std::string> backgrounds = {
    { "round-rect-panel", "RoundRect10" },
    { "panel-top", "RoundRect10" },
    { "panel-fade-gradient", "RoundRect10Thin" },
    { "panel-top-gradient", "RoundRect10" },
    { "title-gradient", "RoundRect10" }
};

std::map<std::string, std::string> objectNames = {
    { "round-rect-panel", "KeyboardWrapper" },
    { "panel-top", "BG" },
    { "panel-fade-gradient", "Background" },
    { "panel-top-gradient", "BG" },
    { "title-gradient", "BG" }
};

std::map<std::string, std::string> objectParentNames = {
    { "round-rect-panel", "Wrapper" },
    { "panel-top", "PracticeButton" },
    { "panel-fade-gradient", "LevelListTableCell" },
    { "panel-top-gradient", "ActionButton" },
    { "title-gradient", "TitleViewController" }
};

template<typename T, typename U>
using Dictionary = System::Collections::Generic::Dictionary_2<T, U>;

SafePtr<Dictionary<StringW, HMUI::ImageView*>> backgroundCache;

using namespace UnityEngine;

namespace BSML {
    void Backgroundable::ApplyBackground(StringW name) {
        if (background) {
            ERROR("Can't apply backgrounds twice!");
            return;
        }

        auto backgroundNameItr = backgrounds.find(name);
        if (backgroundNameItr == backgrounds.end()) {
            ERROR("Unknown background name: {}, Skipping!", name);
            return;
        }

        if (!backgroundCache)
            backgroundCache.emplace(Dictionary<StringW, HMUI::ImageView*>::New_ctor());

        HMUI::ImageView* bgTemplate = nullptr;
        if (!backgroundCache->TryGetValue(name, byref(bgTemplate)) || (!bgTemplate || !bgTemplate->m_CachedPtr.m_value)) {
            if (!bgTemplate || !bgTemplate->m_CachedPtr.m_value) {
                backgroundCache->Remove(name);
            }

            bgTemplate = FindTemplate(name, backgroundNameItr->second);
            backgroundCache->Add(name, bgTemplate);
        }

        background = get_gameObject()->AddComponent<HMUI::ImageView*>();

        //Copy Image: some methods are probably not needed
		background->set_alphaHitTestMinimumThreshold(bgTemplate->get_alphaHitTestMinimumThreshold());
		background->set_color(bgTemplate->get_color());
		background->set_color0(bgTemplate->get_color0());
		background->set_color1(bgTemplate->get_color1());
		background->set_gradient(bgTemplate->get_gradient());
		background->_gradientDirection = bgTemplate->_gradientDirection;
        background->_flipGradientColors = bgTemplate->_flipGradientColors;
        background->_skew = bgTemplate->_skew;
		background->set_eventAlphaThreshold(bgTemplate->get_eventAlphaThreshold());
		background->set_fillAmount(bgTemplate->get_fillAmount());
		background->set_fillCenter(bgTemplate->get_fillCenter());
		background->set_fillClockwise(bgTemplate->get_fillClockwise());
		background->set_fillMethod(bgTemplate->get_fillMethod());
		background->set_fillOrigin(bgTemplate->get_fillOrigin());
		background->set_hideFlags(bgTemplate->get_hideFlags());
		background->set_maskable(bgTemplate->get_maskable());
		background->set_material(bgTemplate->get_material());
		background->set_onCullStateChanged(bgTemplate->get_onCullStateChanged());
		background->set_overrideSprite(bgTemplate->get_overrideSprite());
		background->set_pixelsPerUnitMultiplier(bgTemplate->get_pixelsPerUnitMultiplier());
		background->set_preserveAspect(bgTemplate->get_preserveAspect());
		background->set_raycastTarget(bgTemplate->get_raycastTarget());
		background->set_sprite(bgTemplate->get_sprite());
		background->set_tag(bgTemplate->get_tag());
		background->set_type(bgTemplate->get_type());
		background->set_useGUILayout(bgTemplate->get_useGUILayout());
		background->set_useLegacyMeshGeneration(bgTemplate->get_useLegacyMeshGeneration());
		background->set_useSpriteMesh(bgTemplate->get_useSpriteMesh());

        background->set_enabled(true);
    }

    void Backgroundable::ApplyColor(UnityEngine::Color color) {
        if (!background) {
            ERROR("No background exists yet!");
            return;
        }
        UnityEngine::Color color0{1.0, 1.0, 1.0, background->get_color0().a};
        UnityEngine::Color color1{1.0, 1.0, 1.0, background->get_color1().a};
        color.a = background->get_color().a;

        background->set_gradient(false);
        background->set_color0(color0);
        background->set_color1(color1);
        background->set_color(color);
    }

    void Backgroundable::ApplyGradient(UnityEngine::Color color0, UnityEngine::Color color1) {
        if (!background) {
            ERROR("No background exists yet!");
            return;
        }
        UnityEngine::Color color = {1.0, 1.0, 1.0, background->get_color().a};
        background->set_gradient(true);
        background->set_color(color);
        background->set_color0(color0);
        background->set_color1(color1);
    }

    void Backgroundable::ApplyColor0(UnityEngine::Color color) {
        if (!background) {
            ERROR("No background exists yet!");
            return;
        }

        ApplyGradient(color, background->get_color1());
    }

    void Backgroundable::ApplyColor1(UnityEngine::Color color) {
        if (!background) {
            ERROR("No background exists yet!");
            return;
        }

        ApplyGradient(background->get_color0(), color);
    }


    void Backgroundable::ApplyAlpha(float alpha) {
        if (!background) {
            ERROR("No background exists yet!");
            return;
        }
        auto col = background->get_color();
        col.a = alpha;
        background->set_color(col);
    }

    HMUI::ImageView* Backgroundable::FindTemplate(StringW name, StringW backgroundName) {
        auto objectName = objectNames.find(name)->second;
        auto parentName = objectParentNames.find(name)->second;

        auto images = Resources::FindObjectsOfTypeAll<HMUI::ImageView*>();

        for (auto image : images) {
            auto sprite = image->get_sprite();
            if (!sprite || sprite->get_name() != backgroundName) continue;

            auto parent = image->get_transform()->get_parent();
            if (!parent || parent->get_name() != parentName) continue;

            auto goName = image->get_gameObject()->get_name();
            if (goName != objectName) continue;

            return image;
        }

        return nullptr;
    }
}
