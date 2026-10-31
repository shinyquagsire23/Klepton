#include "BSML/Animations/GIF/GifDecoder.hpp"
#include "logging.hpp"

#include "Helpers/delegates.hpp"

#include "UnityEngine/WaitUntil.hpp"

#include "gif-lib/shared/gif_lib.h"

#include "EasyGifReader.h"

std::string errToString(const EasyGifReader::Error& err) {
    switch (err) {
        default:
        case EasyGifReader::Error::UNKNOWN: return "UNKNOWN";
        case EasyGifReader::Error::INVALID_OPERATION: return "INVALID_OPERATION";
        case EasyGifReader::Error::OPEN_FAILED: return "OPEN_FAILED";
        case EasyGifReader::Error::READ_FAILED: return "READ_FAILED";
        case EasyGifReader::Error::INVALID_FILENAME: return "INVALID_FILENAME";
        case EasyGifReader::Error::NOT_A_GIF_FILE: return "NOT_A_GIF_FILE";
        case EasyGifReader::Error::INVALID_GIF_FILE: return "INVALID_GIF_FILE";
        case EasyGifReader::Error::OUT_OF_MEMORY: return "OUT_OF_MEMORY";
        case EasyGifReader::Error::CLOSE_FAILED: return "CLOSE_FAILED";
        case EasyGifReader::Error::NOT_READABLE: return "NOT_READABLE";
        case EasyGifReader::Error::IMAGE_DEFECT: return "IMAGE_DEFECT";
        case EasyGifReader::Error::UNEXPECTED_EOF: return "UNEXPECTED_EOF";
    }
}

/// @brief makes RGB (0, 0, 0) into transparent pixels
inline uint32_t make_black_transparent(const uint32_t& v) {
    return v >> 8 ? v : 0;
}

namespace BSML {

    custom_types::Helpers::Coroutine GifDecoder::Process(ArrayW<uint8_t> data, std::function<void(AnimationInfo*)> onFinished) {
        co_yield custom_types::Helpers::CoroutineHelper::New(Process(data, onFinished, [](){
            ERROR("Unhandled gif processing error ocurred!");
        }));
        co_return;
    }

    custom_types::Helpers::Coroutine GifDecoder::Process(ArrayW<uint8_t> data, std::function<void(AnimationInfo*)> onFinished, std::function<void()> onError) {
        auto animationInfo = new AnimationInfo();

        il2cpp_utils::il2cpp_aware_thread(
            static_cast<void(*)(ArrayW<uint8_t>, AnimationInfo*, std::function<void()>)>(&GifDecoder::ProcessingThread),
            data, animationInfo, onError
        ).detach();

        while (!animationInfo->isInitialized) co_yield nullptr;

        if (onFinished)
            onFinished(animationInfo);
        else
            ERROR("Nullptr onFinished given!");
        co_return;
    }

    void GifDecoder::ProcessingThread(ArrayW<uint8_t> gifData, AnimationInfo* animationInfo) {
        ProcessingThread(gifData, animationInfo, [](){
            ERROR("Unhandled Gif Processing Error ocurred!");
        });
    }

    void GifDecoder::ProcessingThread(ArrayW<uint8_t> gifData, AnimationInfo* animationInfo, std::function<void()> onError) {
        DEBUG("Open gif");
        try {
            auto gifReader = EasyGifReader::openMemory(gifData->_values, gifData.size());
            int width = gifReader.width(), height = gifReader.height(), frameCount = gifReader.frameCount();

            animationInfo->frameCount = frameCount;
            animationInfo->isInitialized = true;
            animationInfo->width = width;
            animationInfo->height = height;

            DEBUG("iterating gif frames");
            for (const auto& gifFrame : gifReader) {
                auto outputFrameInfo = animationInfo->AddFrame(gifFrame.width(), gifFrame.height());

                const uint8_t* pixels = (const uint8_t*)gifFrame.pixels();
                // get end of the data
                uint8_t* colorData = outputFrameInfo->colors.ptr()->_values + outputFrameInfo->colors->get_Length();
                int height = gifFrame.height();
                int rowSize = sizeof(uint32_t) * gifFrame.width();
                // we need to iterate the given data in reverse due to unity's texture system
                for (int y = 0; y < height; y++) {
                    // pre-decrement because we start at end of data
                    colorData -= rowSize;
                    // just copy all the data as is
                    //memcpy(colorData, pixels, rowSize);
                    // make black pixels transparent
                    std::transform((uint32_t*)pixels, (uint32_t*)(pixels + rowSize), (uint32_t*)colorData, make_black_transparent);
                    pixels += rowSize;
                }

                // delay in millis
                outputFrameInfo->delay = gifFrame.rawDuration().milliseconds();

                // increase decoded frame count
                animationInfo->decodedFrames++;
            }
        } catch (EasyGifReader::Error gifError) {
            ERROR("Gif error: {}", errToString(gifError));
            if (onError) onError();
        }
    }
}
