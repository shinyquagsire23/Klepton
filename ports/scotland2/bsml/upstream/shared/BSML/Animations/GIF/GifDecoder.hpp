#pragma once

#include "../../../_config.h"

#include "../AnimationInfo.hpp"
#include "beatsaber-hook/shared/utils/typedefs.h"
#include "custom-types/shared/coroutine.hpp"

namespace BSML {
    class BSML_EXPORT GifDecoder {
        public:
            static custom_types::Helpers::Coroutine Process(ArrayW<uint8_t> data, std::function<void(AnimationInfo*)> onFinished, std::function<void()> onError);
            static custom_types::Helpers::Coroutine Process(ArrayW<uint8_t> data, std::function<void(AnimationInfo*)> onFinished);
            static void ProcessingThread(ArrayW<uint8_t> gifData, AnimationInfo* animationInfo);
            static void ProcessingThread(ArrayW<uint8_t> gifData, AnimationInfo* animationInfo, std::function<void()> onError);
    };
}
