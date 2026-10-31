#pragma once

#include "../../_config.h"

#include "../TransformWrapper.hpp"
#include "HMUI/ImageView.hpp"
#include "UnityEngine/Sprite.hpp"
#include "UnityEngine/UI/RawImage.hpp"
#include "UnityEngine/Vector2.hpp"
#include "UnityEngine/Texture2D.hpp"
#include "../../BSML/Components/ClickableImage.hpp"

namespace BSML::Lite {
    /// @brief Create an image
    /// @param parent what to parent it to
    /// @param sprite the sprite to display
    /// @param anchoredPosition position of the anchor relative to the parent
    /// @param sizeDelta how much smaller this thing is relative to the parent
    /// @return the created image view
    BSML_EXPORT HMUI::ImageView* CreateImage(const TransformWrapper& parent, UnityEngine::Sprite* sprite, UnityEngine::Vector2 anchoredPosition = {0, 0}, UnityEngine::Vector2 sizeDelta = {0, 0});

    /// @brief Create an image
    /// @param parent what to parent it to
    /// @param sprite the sprite to display
    /// @param onClick callback ran when image is clicked
    /// @param anchoredPosition position of the anchor relative to the parent
    /// @param sizeDelta how much smaller this thing is relative to the parent
    /// @return the created image view
    BSML_EXPORT BSML::ClickableImage* CreateClickableImage(const TransformWrapper& parent, UnityEngine::Sprite* sprite, std::function<void()> onClick = nullptr, UnityEngine::Vector2 anchoredPosition = {0, 0}, UnityEngine::Vector2 sizeDelta = {0, 0});

    /// @brief Create a raw image
    /// @param parent what to parent it to
    /// @param texture the texture to display
    /// @param anchoredPosition position of the anchor relative to the parent
    /// @param sizeDelta how much smaller this thing is relative to the parent
    /// @return the created image view
    BSML_EXPORT UnityEngine::UI::RawImage* CreateRawImage(const TransformWrapper& parent, UnityEngine::Texture* texture, UnityEngine::Vector2 anchoredPosition = {0, 0}, UnityEngine::Vector2 sizeDelta = {0, 0});

    /// @brief loads a sprite from a file path
    /// @param filePath path
    /// @return created sprite
    BSML_EXPORT UnityEngine::Sprite* FileToSprite(const std::string_view& filePath);

    /// @brief make a sprite from a texture
    /// @param tex texture to use
    /// @return created sprite
    BSML_EXPORT UnityEngine::Sprite* TextureToSprite(UnityEngine::Texture2D* tex);

    /// @brief make a sprite from a base64 encoded image
    /// @param base64Str the encoded string
    /// @return created sprite
    BSML_EXPORT UnityEngine::Sprite* Base64ToSprite(const std::string_view& base64Str);

    /// @brief Get a sprite from a vector of bytes
    /// @param bytes bytes to load from
    /// @return created sprite
    BSML_EXPORT UnityEngine::Sprite* VectorToSprite(const std::vector<uint8_t>& bytes);

    /// @brief Get a sprite from a C# array of bytes
    /// @param bytes bytes to load from
    /// @return created sprite
    BSML_EXPORT UnityEngine::Sprite* ArrayToSprite(ArrayW<uint8_t> bytes);
}
