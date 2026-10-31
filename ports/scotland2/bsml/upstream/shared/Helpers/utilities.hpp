#pragma once

#include "../_config.h"
#include "UnityEngine/Texture2D.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Sprite.hpp"
#include "UnityEngine/Color.hpp"
#include "UnityEngine/Color32.hpp"
#include "UnityEngine/UI/Image.hpp"
#include "System/Uri.hpp"

namespace BSML::Utilities {
    enum class ImageLoadError {
        None,
        NetworkError,
        GetDataError,
        GifParsingError,
        Unknown,
        ImageParsingError
    };

    /// @brief Finds a sprite by name.
    BSML_EXPORT UnityEngine::Sprite* FindSpriteCached(StringW name);

    /// @brief Finds a texture by name.
    BSML_EXPORT UnityEngine::Texture* FindTextureCached(StringW name);

    /// @brief parses a HTML color from given string view
    /// @param str the string view
    /// @return the color, or nullopt
    BSML_EXPORT std::optional<UnityEngine::Color> ParseHTMLColorOpt(std::string_view str);

    /// @brief parses a HTML color from given string view
    /// @param str the string view
    /// @return the color, or white if invalid
    BSML_EXPORT UnityEngine::Color ParseHMTMLColor(std::string_view str);

    /// @brief parses a HTML color from given string view
    /// @param str the string view
    /// @return the color32, or nullopt
    BSML_EXPORT std::optional<UnityEngine::Color32> ParseHTMLColor32Opt(std::string_view str);

    /// @brief parses a HTML color from given string view
    /// @param str the string view
    /// @return the color32, or white if invalid
    BSML_EXPORT UnityEngine::Color32 ParseHTMLColor32(std::string_view str);

    struct BSML_EXPORT ScaleOptions {
        int width;
        int height;
        bool maintainRatio;
        bool shouldScale;

        ScaleOptions(int width = 0, int height = 0, bool maintainRatio = true, bool shouldScale = false)
            : width(width)
            , height(height)
            , maintainRatio(maintainRatio)
            , shouldScale(shouldScale)
        {}
    };

    /// @brief Downscales a texture using a rendertexture (I know, not the most efficient way to do it)
    /// @param tex the texture to copy and downscale
    /// @param options the options to use for downscaling
    /// @return downscaled texture, or tex if it was not scaled
    BSML_EXPORT UnityEngine::Texture2D* DownScaleTexture(UnityEngine::Texture2D* tex, const ScaleOptions& options);

    /// @brief Downscales a sprite using DownScaleTexture
    /// @param sprite the sprite to copy and downscale
    /// @param options the options to use for downscaling
    /// @return downscaled sprite
    BSML_EXPORT UnityEngine::Sprite* DownScaleSprite(UnityEngine::Sprite* sprite, const ScaleOptions& options);

    /// @brief Removes an image from the internal cache if it exists, make sure it's not used anymore!
    /// @param path the image to remove
    /// @return whether the path was in the cache
    BSML_EXPORT bool RemoveImage(StringW path);

    /// @brief sets the sprite from a path, this does not yet bother with animated textures so it will only work with base textures
    /// @param image the image to set the sprite on
    /// @param path the path, basegame name, or URI to an image
    BSML_EXPORT void SetImage(UnityEngine::UI::Image* image, StringW path);

    /// @brief sets the sprite from a path, this does not yet bother with animated textures so it will only work with base textures
    /// @param image the image to set the sprite on
    /// @param path the path, basegame name, or URI to an image
    /// @param cached whether to cache the resulting sprite or not
    BSML_EXPORT void SetImage(UnityEngine::UI::Image* image, StringW path, bool cached);

    /// @brief sets the sprite from a path, this does not yet bother with animated textures so it will only work with base textures
    /// @param image the image to set the sprite on
    /// @param path the path, basegame name, or URI to an image
    /// @param loadingAnimation currently unused, but might eventually be used for a loading animation while stuff is downloading
    /// @param scaleOptions the scale options to use, if any
    /// @param onFinished a callback to call when the image is finished loading, if any
    BSML_EXPORT void SetImage(UnityEngine::UI::Image* image, StringW path, bool loadingAnimation, ScaleOptions scaleOptions, std::function<void()> onFinished = nullptr);

    /// @brief sets the sprite from a path, this does not yet bother with animated textures so it will only work with base textures
    /// @param image the image to set the sprite on
    /// @param path the path, basegame name, or URI to an image
    /// @param loadingAnimation currently unused, but might eventually be used for a loading animation while stuff is downloading
    /// @param scaleOptions the scale options to use, if any
    /// @param onFinished a callback to call when the image is finished loading, if any
    /// @param onError a callback to call when there was an error
    BSML_EXPORT void SetImage(UnityEngine::UI::Image* image, StringW path, bool loadingAnimation, ScaleOptions scaleOptions, std::function<void()> onFinished, std::function<void(ImageLoadError)> onError);

    /// @brief sets the sprite from a path, this does not yet bother with animated textures so it will only work with base textures
    /// @param image the image to set the sprite on
    /// @param path the path, basegame name, or URI to an image
    /// @param loadingAnimation currently unused, but might eventually be used for a loading animation while stuff is downloading
    /// @param scaleOptions the scale options to use, if any
    /// @param cached whether to set the image and cache it, or to skip caching it if loaded newly. only works for static images, not for animated images
    /// @param onFinished a callback to call when the image is finished loading, if any
    /// @param onError a callback to call when there was an error
    BSML_EXPORT void SetImage(UnityEngine::UI::Image* image, StringW path, bool loadingAnimation, ScaleOptions scaleOptions, bool cached, std::function<void()> onFinished, std::function<void(ImageLoadError)> onError);

    /// @brief function to get data at a URI, this is not blocking as it dispatches a coroutine
    /// @param uri the URI to get data from
    /// @param onFinished what to do with the downloaded bytes, passed nullptr on fail
    BSML_EXPORT void DownloadData(StringW uri, std::function<void(ArrayW<uint8_t>)> onFinished = nullptr);

    /// @brief function to get data from the key to data cache, this is blocking due to the data being available immediately
    /// @param key the key to get the data with
    /// @param onFinished what to do with the downloaded bytes, onFinished is ran with a nullptr argument if not found
    BSML_EXPORT void GetData(StringW key, std::function<void(ArrayW<uint8_t>)> onFinished = nullptr);

    /// @brief Function to load a sprite from a data array
    /// @param data the data to load the sprite from
    /// @param pixelsPerUnit the pixels per unit
    /// @return created sprite
    BSML_EXPORT UnityEngine::Sprite* LoadSpriteRaw(ArrayW<uint8_t> data, float pixelsPerUnit = 100.0f);

    /// @brief Function to load a sprite from a texture
    /// @param texture the texture to use
    /// @param pixelsPerUnit the pixels per unit
    /// @return created sprite
    BSML_EXPORT UnityEngine::Sprite* LoadSpriteFromTexture(UnityEngine::Texture2D* texture, float pixelsPerUnit = 100.0f);

    /// @brief Function to load a texture from a data array
    /// @param data the data to load the texture from
    /// @return created texture
    BSML_EXPORT UnityEngine::Texture2D* LoadTextureRaw(ArrayW<uint8_t> data);

    /// @brief function to copy the values from other to comp
    /// @param comp the component to copy to
    /// @param other the component to copy from
    /// @return comp, or nullptr if types are different
    BSML_EXPORT UnityEngine::Component* GetCopyOfComponent(UnityEngine::Component* comp, UnityEngine::Component* other);

    /// @brief function to copy the values from other to comp, casted to the correct type
    /// @tparam T the type of the component
    /// @param comp the component to copy to
    /// @param other the component to copy from
    /// @return comp, or nullptr if types are different
    template<typename T>
    requires(std::is_convertible_v<T, UnityEngine::Component*>)
    T GetCopyOf(UnityEngine::Component* comp, T other) {
        return reinterpret_cast<T>(GetCopyOfComponent(comp, other));
    }

    /// @brief function to copy a component onto gameObject
    /// @tparam T the type of the component
    /// @param gameObject the gameObject to add the component to
    /// @param original the component to copy from
    /// @return newly added component
    template<typename T>
    requires(std::is_convertible_v<T, UnityEngine::Component*>)
    T AddComponent(UnityEngine::GameObject* gameObject, T original) {
        return GetCopyOf<T>(gameObject->AddComponent<T>(), original);
    }

    namespace ImageResources {
        /// @brief method that gets a blank sprite
        /// @return black texture to sprite
        BSML_EXPORT UnityEngine::Sprite* GetBlankSprite();

        /// @brief method that gets a white pixel sprite
        /// @return white pixel to sprite
        BSML_EXPORT UnityEngine::Sprite* GetWhitePixel();
    }
}

#if __has_include("fmt/core.h")
#include <fmt/core.h>
template <> struct fmt::formatter<::BSML::Utilities::ImageLoadError> : formatter<string_view> {
    // parse is inherited from formatter<string_view>.
    template <typename FormatContext>
    auto format(::BSML::Utilities::ImageLoadError e, FormatContext& ctx) const {
        switch(e) {
            case ::BSML::Utilities::ImageLoadError::None:
                return formatter<string_view>::format("None", ctx);
            case ::BSML::Utilities::ImageLoadError::NetworkError:
                return formatter<string_view>::format("NetworkError", ctx);
            case ::BSML::Utilities::ImageLoadError::GetDataError:
                return formatter<string_view>::format("GetDataError", ctx);
            case ::BSML::Utilities::ImageLoadError::GifParsingError:
                return formatter<string_view>::format("GifParsingError", ctx);
            default: [[fallthrough]];
            case ::BSML::Utilities::ImageLoadError::Unknown:
                return formatter<string_view>::format("Unknown", ctx);
        }
    }
};
#endif
