#include "BSML/Animations/AnimationControllerData.hpp"
#include "BSML/MainThreadScheduler.hpp"
#include "logging.hpp"

#include "Helpers/utilities.hpp"

#include "UnityEngine/SpriteMeshType.hpp"
#include "UnityEngine/Texture2D.hpp"
#include "UnityEngine/Vector2.hpp"
#include "UnityEngine/Vector4.hpp"
#include "System/Object.hpp"
#include <chrono>

DEFINE_TYPE(BSML, AnimationControllerData);

namespace BSML {
    AnimationControllerData* AnimationControllerData::Make_new(UnityEngine::Texture2D* tex, ArrayW<UnityEngine::Rect> uvs, ArrayW<float> delays) {
        auto self = AnimationControllerData::New_ctor();
        self->animationStateUpdaters = {};

        self->_isPlaying = true;
        self->isDelayConsistent = true;
        auto time = std::chrono::system_clock::now();
        auto milis = std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch());
        self->lastSwitch = milis.count();

        self->sprites = ArrayW<UnityEngine::Sprite*>(uvs.size());
        float firstDelay = -1;
        int texWidth = tex->get_width();
        int texHeight = tex->get_height();

        for (int i = 0; i < uvs.size(); i++) {
            UnityEngine::Rect& uv = uvs[i];
            self->sprites[i] = UnityEngine::Sprite::Create(
                tex,
                UnityEngine::Rect(
                    uv.m_XMin * texWidth,
                    uv.m_YMin * texHeight,
                    uv.m_Width * texWidth,
                    uv.m_Height * texHeight
                ),
                {0, 0},
                100.0f,
                0,
                UnityEngine::SpriteMeshType::Tight,
                {0, 0, 0, 0},
                false
            );

            if (i == 0) {
                firstDelay = delays[i];
            }

            if (delays[i] != firstDelay) {
                self->isDelayConsistent = false;
            }
        }

        self->sprite = Utilities::LoadSpriteFromTexture(tex);
        self->uvs = uvs;
        self->delays = delays;

        return self;
    }

    void AnimationControllerData::Finalize() {
        BSML::MainThreadScheduler::Schedule([sprite = this->sprite](){
            if (sprite && sprite->m_CachedPtr.m_value) {
                auto tex = sprite->texture;
                if (tex && tex->m_CachedPtr.m_value) {
                    UnityEngine::Object::DestroyImmediate(tex);
                }
                UnityEngine::Object::DestroyImmediate(sprite);
            }
        });
        sprite = nullptr;

        auto objectFinalize = il2cpp_utils::il2cpp_type_check::MetadataGetter<&System::Object::Finalize>::methodInfo();
        il2cpp_utils::RunMethodRethrow<void, false>(this, objectFinalize);

        this->~AnimationControllerData();
    }

    ListW<UnityEngine::UI::Image*> AnimationControllerData::get_activeImages() {
        if (!_activeImages) {
            _activeImages = ListW<UnityEngine::UI::Image*>::New();
        }
        return _activeImages;
    }

    bool AnimationControllerData::get_isPlaying() {
        return _isPlaying;
    }

    void AnimationControllerData::set_isPlaying(bool value) {
        _isPlaying = value;
    }

    void AnimationControllerData::CheckFrame(unsigned long long now) {
        if (activeImages.size() == 0) return;

        auto diffMs = (now - lastSwitch);
        if (diffMs < delays[uvIndex]) return;
        // VV Bump animations with consistently 10ms or lower frame timings to 100ms
        if (isDelayConsistent && delays[uvIndex] <= 10 && diffMs < 100) return;

        lastSwitch = now;
        do {
            uvIndex++;
            if (uvIndex >= uvs.size()) uvIndex = 0;
        } while (!isDelayConsistent && delays[uvIndex] == 0);

        for (auto image : activeImages) {
            image->set_sprite(sprites[uvIndex]);
        }
    }

    bool AnimationControllerData::IsBeingUsed() {
        return !animationStateUpdaters.empty(); // if no anim updaters exist with this data, it's not being used
    }

    bool AnimationControllerData::Add(AnimationStateUpdater* animationStateUpdater) {
        auto itr = animationStateUpdaters.find(animationStateUpdater);
        if (itr == animationStateUpdaters.end()) {
            animationStateUpdaters.emplace(animationStateUpdater);
            return true;
        } else {
            ERROR("Trying to register {} twice!", fmt::ptr(animationStateUpdater));
            return false;
        }
    }

    bool AnimationControllerData::Remove(AnimationStateUpdater* animationStateUpdater) {
        auto itr = animationStateUpdaters.find(animationStateUpdater);
        if (itr != animationStateUpdaters.end()) {
            animationStateUpdaters.erase(itr);
            return true;
        } else {
            ERROR("Trying to remove {} twice!", fmt::ptr(animationStateUpdater));
            return false;
        }

    }
}
