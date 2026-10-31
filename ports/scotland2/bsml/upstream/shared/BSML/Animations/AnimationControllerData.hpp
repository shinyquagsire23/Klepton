#pragma once

#include "custom-types/shared/macros.hpp"
#include "UnityEngine/Sprite.hpp"
#include "UnityEngine/Rect.hpp"
#include "UnityEngine/Material.hpp"
#include "UnityEngine/UI/Image.hpp"

namespace BSML {
    class AnimationStateUpdater;
}

DECLARE_CLASS_CODEGEN(BSML, AnimationControllerData, System::Object) {
    DECLARE_INSTANCE_FIELD(UnityEngine::Sprite*, sprite);
    DECLARE_INSTANCE_FIELD(int, uvIndex);
    DECLARE_INSTANCE_FIELD(ArrayW<UnityEngine::Rect>, uvs);
    DECLARE_INSTANCE_FIELD(ArrayW<float>, delays);
    DECLARE_INSTANCE_FIELD(ArrayW<UnityEngine::Sprite*>, sprites);
    DECLARE_INSTANCE_FIELD(UnityEngine::Material*, animMaterial);
    DECLARE_INSTANCE_FIELD(bool, isDelayConsistent);
    DECLARE_INSTANCE_FIELD(ListW<UnityEngine::UI::Image*>, _activeImages);
    DECLARE_INSTANCE_METHOD(ListW<UnityEngine::UI::Image*>, get_activeImages);

    DECLARE_INSTANCE_METHOD(bool, IsBeingUsed);

    DECLARE_INSTANCE_FIELD(bool, _isPlaying);
    DECLARE_INSTANCE_METHOD(bool, get_isPlaying);
    DECLARE_INSTANCE_METHOD(void, set_isPlaying, bool value);

    DECLARE_DEFAULT_CTOR();
    DECLARE_OVERRIDE_METHOD_MATCH(void, Finalize, &System::Object::Finalize);
    public:
        __declspec(property(get=get_activeImages)) ListW<UnityEngine::UI::Image*> activeImages;
        bool Add(AnimationStateUpdater* animationStateUpdater);
        bool Remove(AnimationStateUpdater* animationStateUpdater);

        static AnimationControllerData* Make_new(UnityEngine::Texture2D* tex, ArrayW<UnityEngine::Rect> uvs, ArrayW<float> delays);
        void CheckFrame(unsigned long long now);
    private:
        unsigned long long lastSwitch;
        std::set<AnimationStateUpdater*> animationStateUpdaters;
};