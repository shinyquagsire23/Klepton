#include "BSML/Animations/AnimationLoader.hpp"
#include "BSML/Animations/AnimationInfo.hpp"
#include "BSML/Animations/GIF/GifDecoder.hpp"
#include "logging.hpp"

#include "UnityEngine/SystemInfo.hpp"
#include "UnityEngine/Time.hpp"
#include "UnityEngine/WaitUntil.hpp"
#include "UnityEngine/TextureWrapMode.hpp"
#include "UnityEngine/TextureFormat.hpp"
#include "UnityEngine/ImageConversion.hpp"
#include "UnityEngine/Resources.hpp"
#include "System/IO/File.hpp"

#include "Helpers/delegates.hpp"

#include "BSML/SharedCoroutineStarter.hpp"
#include "System/Func_1.hpp"
#include <mutex>

DEFINE_TYPE(BSML, AnimationLoader);

namespace BSML {
    int get_atlasSizeLimit() {
        using GetMaxTextureSize = function_ptr_t<int>;
        static auto getMaxTextureSize = reinterpret_cast<GetMaxTextureSize>(il2cpp_functions::resolve_icall("UnityEngine.SystemInfo::GetMaxTextureSize"));
        static int maxSize = getMaxTextureSize();
        return maxSize >= 4096 ? 4096 : maxSize;
    }

    void AnimationLoader::Process(AnimationType type, ArrayW<uint8_t> data, std::function<void(UnityEngine::Texture2D*, ArrayW<UnityEngine::Rect>, ArrayW<float>)> onProcessed) {
        Process(type, data, onProcessed, [](){
            ERROR("Error happened while processing Animation, and error was not handled!");
        });
    }

    void AnimationLoader::Process(AnimationType type, ArrayW<uint8_t> data, std::function<void(UnityEngine::Texture2D*, ArrayW<UnityEngine::Rect>, ArrayW<float>)> onProcessed, std::function<void()> onError) {
        auto sharedStarter = BSML::SharedCoroutineStarter::get_instance();
        DEBUG("Starting animation decode");
        switch (type) {
            case AnimationType::GIF:
                sharedStarter->StartCoroutine(custom_types::Helpers::CoroutineHelper::New(
                    GifDecoder::Process(
                        data,
                        [sharedStarter, onProcessed](auto animationInfo){
                            DEBUG("Processed Data, processing animation info");
                            sharedStarter->StartCoroutine(custom_types::Helpers::CoroutineHelper::New(ProcessAnimationInfo(animationInfo, onProcessed)));
                        },
                        onError
                    )));
                break;
            case AnimationType::APNG:
                // TODO: implement APNG support at some point?
                if (onError) onError();
                break;
            default:
                break;
        }
    }

    custom_types::Helpers::Coroutine AnimationLoader::ProcessAnimationInfo(AnimationInfo* animationInfo, std::function<void(UnityEngine::Texture2D*, ArrayW<UnityEngine::Rect>, ArrayW<float>)> onProcessed) {
        DEBUG("ProcessAnimInfo");
        int textureSize = get_atlasSizeLimit(), width = 0, height = 0;
        SafePtr<Array<UnityEngine::Texture2D*>> textureListSafe = Array<UnityEngine::Texture2D*>::NewLength(animationInfo->frameCount);
        ArrayW<UnityEngine::Texture2D*> textureList(textureListSafe.ptr());
        SafePtr<Array<float>> delaysSafe = Array<float>::NewLength(animationInfo->frameCount);
        ArrayW<float> delays(delaysSafe.ptr());
        float lastThrottleTime = UnityEngine::Time::get_realtimeSinceStartup();

        int gifWidth = 0, gifHeight = 0;
        for (int currentFrameIndex = 0; currentFrameIndex < animationInfo->frameCount; currentFrameIndex++) {
            DEBUG("Frame {}", currentFrameIndex);

            // while the decoded frame count is larger or equal to the size of the vector, we can't be sure we're done decoding that frame
            // decodedframes is indexed starting at 1 since it's a count, so <=
            bool throttled = false;
            while (animationInfo->decodedFrames <= currentFrameIndex) {
                co_yield nullptr;
                throttled = true;
            }
            if (throttled) lastThrottleTime = UnityEngine::Time::get_realtimeSinceStartup();
            auto currentFrameInfo = animationInfo->PopNextFrame();

            if (currentFrameIndex == 0) {
                gifWidth = currentFrameInfo->width;
                gifHeight = currentFrameInfo->height;
                textureSize = GetTextureSize(animationInfo);
            }

            delays[currentFrameIndex] = currentFrameInfo->delay;

            auto frameTexture = UnityEngine::Texture2D::New_ctor(currentFrameInfo->width, currentFrameInfo->height, UnityEngine::TextureFormat::RGBA32, false);
            frameTexture->hideFlags = UnityEngine::HideFlags::DontSave; // Avoids unity GC
            frameTexture->set_wrapMode(UnityEngine::TextureWrapMode::Clamp);
            frameTexture->LoadRawTextureData(currentFrameInfo->colors.ptr());

            textureList[currentFrameIndex] = frameTexture;
            if (UnityEngine::Time::get_realtimeSinceStartup() > lastThrottleTime + 0.0005f) {
                co_yield nullptr;
                lastThrottleTime = UnityEngine::Time::get_realtimeSinceStartup();
            }
        }

        SafePtrUnity<UnityEngine::Texture2D> resultTexture = UnityEngine::Texture2D::New_ctor(gifWidth, gifHeight);

        // note to self, no longer readable = true means you can't encode the texture to png!
        // Warning: If this will ever crash, it means that one of the
        // textures in the array has been garbage collected and we need to attach
        // them to a gameobject to prevent that
        DEBUG("Packing gif textures");
        SafePtr<Array<::UnityEngine::Rect>> atlasSafe = static_cast<Array<::UnityEngine::Rect>*>(resultTexture->PackTextures(textureList, 2, textureSize, true));
        ArrayW<::UnityEngine::Rect> atlas(atlasSafe.ptr());
        // cleanup
        for (auto t : textureList) {
            if (t && t->m_CachedPtr.m_value)
                UnityEngine::Object::DestroyImmediate(t);
        }

        if (onProcessed)
            onProcessed(resultTexture.ptr(), atlas, delays);

        // we are now done with the animation info
        delete animationInfo;
        co_return;
    }

    int AnimationLoader::GetTextureSize(AnimationInfo* animationInfo) {
        int testNum = 2, numFramesInRow = 0, numFramesInColumn = 0;

        while (true) {
            int numFrames = animationInfo->frameCount;

            if ((numFrames % testNum) != 0) {
                numFrames += numFrames % testNum;
            }

            numFramesInRow = std::max(numFrames / testNum, 1);
            numFramesInColumn = numFrames / numFramesInRow;
            if (numFramesInRow <= numFramesInColumn) break;
            testNum += 2;
        }

        int textureWidth = std::clamp(numFramesInRow * animationInfo->width, 0, get_atlasSizeLimit());
        int textureHeight = std::clamp(numFramesInColumn * animationInfo->height, 0, get_atlasSizeLimit());
        return std::max(textureWidth, textureHeight);
    }
}
