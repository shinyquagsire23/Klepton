#pragma once

#include "../../_config.h"
// this is just included because otherwise the SafePtr complains about conversion to unity object
#include "beatsaber-hook/shared/utils/typedefs.h"
#include "UnityEngine/Object.hpp"

#include <memory>
#include <queue>

namespace BSML {
    class FrameInfo;
    class BSML_EXPORT AnimationInfo {
        public:
            ~AnimationInfo();
            bool isInitialized = false;

            /// @brief amount of frames in the animation, not neccesarily the amount of frames you can safely get
            int frameCount = 0;

            /// @brief how many frames have been fully decoded
            std::atomic<std::size_t> decodedFrames;

            /// @brief width of the animation
            int width = 0;
            /// @brief height of the animation
            int height = 0;

            /// @brief gets the next decoded frame
            std::shared_ptr<FrameInfo> PopNextFrame() {
                std::lock_guard<std::mutex> lock(framesAccessMutex);
                auto frame = frames.front();
                frames.pop();
                return frame;
            }

            /// @brief adds a frame to the frames vector, though adding frames after frameCount should be impossible
            std::shared_ptr<FrameInfo> AddFrame(int width, int height) {
                std::lock_guard<std::mutex> lock(framesAccessMutex);
                return frames.emplace(std::make_shared<FrameInfo>(width, height));
            }

        private:
            std::mutex framesAccessMutex;
            std::queue<std::shared_ptr<FrameInfo>> frames;
    };

    class BSML_EXPORT FrameInfo {
        public:
            int width, height, bpp;
            SafePtr<Array<uint8_t>> colors;
            int delay;
            FrameInfo(int width, int height, int bpp = 4);
    };
}
