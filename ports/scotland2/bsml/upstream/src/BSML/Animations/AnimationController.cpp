#include "BSML/Animations/AnimationController.hpp"
#include "BSML/Animations/AnimationControllerData.hpp"
#include "BSML/Animations/AnimationLoader.hpp"
#include "logging.hpp"
#include "assets.hpp"

#include "UnityEngine/GameObject.hpp"
#include "System/Collections/Generic/KeyValuePair_2.hpp"
#include <chrono>

DEFINE_TYPE(BSML, AnimationController);

namespace BSML {
    SafePtrUnity<AnimationController> AnimationController::instance;
    AnimationController* AnimationController::get_instance() {
        if (!instance) {
            instance = UnityEngine::GameObject::New_ctor()->AddComponent<AnimationController*>();
            instance->get_gameObject()->set_name("AnimationController");
            UnityEngine::Object::DontDestroyOnLoad(instance->get_gameObject());
            instance->InitializeLoadingAnimation();
        }
        return instance.ptr();
    }

    void AnimationController::ctor() {
        registeredAnimations = StringToAnimDataDictionary::New_ctor();
    }

    bool AnimationController::Unregister(StringW key) {
        AnimationControllerData* data;
        if (TryGetAnimationControllerData(key, data)) { // we found it
            if (CanUnregister(data)) { // we can unregister
                registeredAnimations->Remove(key);
                return true;
            } else { // we're not allowed to unregister
                INFO("Was not able to unregister key {} because CanUnregister returned false, are you still using this animation somewhere?", key);
                return false;
            }
        } else { // not registered
            INFO("Can't unregister key {} as it's not registered", key);
            return false;
        }
    }

    bool AnimationController::CanUnregister(AnimationControllerData* animationData) {
        if (animationData == loadingAnimation) return false;
        return animationData ? !animationData->IsBeingUsed() : true;
    }

    bool AnimationController::TryGetAnimationControllerData(StringW key, AnimationControllerData*& out) {
        union {
            System::Object* data = nullptr;
            AnimationControllerData* animationData;
        };

        if (registeredAnimations->TryGetValue(key, byref(data))) {
            out = animationData;
            return true;
        }
        out = nullptr;
        return false;
    }

    AnimationControllerData* AnimationController::Register(StringW key, UnityEngine::Texture2D* texture, ArrayW<UnityEngine::Rect> uvs, ArrayW<float> delays) {
        DEBUG("Registering {}", key);
        AnimationControllerData* animationData;
        if (!TryGetAnimationControllerData(key, animationData)) {
            animationData = AnimationControllerData::Make_new(texture, uvs, delays);
            registeredAnimations->Add(key, animationData);
        } else {
            INFO("key {} was already registered, destroying texture and returning", key);
            UnityEngine::Object::Destroy(texture);
        }
        return animationData;
    }

    void AnimationController::InitializeLoadingAnimation() {
        BSML::AnimationLoader::Process(AnimationLoader::AnimationType::GIF, Assets::loading_gif,
            [this](UnityEngine::Texture2D* tex, ArrayW<UnityEngine::Rect> uvs, ArrayW<float> delays){
                loadingAnimation = AnimationControllerData::Make_new(tex, uvs, delays);
            }
        );
    }

    void AnimationController::Update() {
        auto time = std::chrono::system_clock::now();
        auto milis = std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch());
        auto now = milis.count();
        auto enumerator = registeredAnimations->GetEnumerator();

        std::set<std::string> toUnregister = {};
        while (enumerator.MoveNext()) {
            auto curItr = enumerator.get_Current();
            auto current = reinterpret_cast<AnimationControllerData*>(curItr.get_Value());

            // if it's not being used, skip it and add it to the list to unregister
            if (!current->IsBeingUsed()) {
                toUnregister.emplace(curItr.get_Key());
                continue;
            }

            if (current->get_isPlaying()) {
                current->CheckFrame(now);
            }
        }

        for (auto& v : toUnregister) {
            Unregister(v);
        }
    }
}
