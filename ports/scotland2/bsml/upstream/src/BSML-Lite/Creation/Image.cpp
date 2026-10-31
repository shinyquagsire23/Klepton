#include "BSML-Lite/Creation/Image.hpp"

#define protected public
#include "BSML/Tags/ImageTag.hpp"
#include "BSML/Tags/RawImageTag.hpp"
#include "BSML/Tags/ClickableImageTag.hpp"
#undef protected

#include "UnityEngine/RectTransform.hpp"
#include "UnityEngine/SpriteMeshType.hpp"
#include "UnityEngine/ImageConversion.hpp"
#include "UnityEngine/TextureFormat.hpp"
#include "UnityEngine/Rect.hpp"
#include "UnityEngine/Vector4.hpp"
#include "cppcodec/base64_rfc4648.hpp"
#include <fstream>

namespace BSML::Lite {
    HMUI::ImageView* CreateImage(const TransformWrapper& parent, UnityEngine::Sprite* sprite, UnityEngine::Vector2 anchoredPosition, UnityEngine::Vector2 sizeDelta) {
        auto go = BSML::ImageTag{}.CreateObject(parent);
        auto imageView = go->GetComponent<HMUI::ImageView*>();
        imageView->set_sprite(sprite);

        auto rect = go->GetComponent<UnityEngine::RectTransform*>();
        rect->set_anchoredPosition(anchoredPosition);
        rect->set_sizeDelta(sizeDelta);
        return imageView;
    }

    BSML::ClickableImage* CreateClickableImage(const TransformWrapper& parent, UnityEngine::Sprite* sprite, std::function<void()> onClick, UnityEngine::Vector2 anchoredPosition, UnityEngine::Vector2 sizeDelta) {
        auto go = BSML::ClickableImageTag{}.CreateObject(parent);
        auto clickableImage = go->GetComponent<BSML::ClickableImage*>();
        clickableImage->set_sprite(sprite);

        auto rect = go->GetComponent<UnityEngine::RectTransform*>();
        rect->set_anchoredPosition(anchoredPosition);
        rect->set_sizeDelta(sizeDelta);

        if (onClick) clickableImage->onClick += {onClick};
        return clickableImage;
    }

    UnityEngine::UI::RawImage* CreateRawImage(const TransformWrapper& parent, UnityEngine::Texture* texture, UnityEngine::Vector2 anchoredPosition, UnityEngine::Vector2 sizeDelta) {
        auto go = BSML::ImageTag{}.CreateObject(parent);
        auto rawImage = go->GetComponent<UnityEngine::UI::RawImage*>();
        rawImage->set_texture(texture);

        auto rect = go->GetComponent<UnityEngine::RectTransform*>();
        rect->set_anchoredPosition(anchoredPosition);
        rect->set_sizeDelta(sizeDelta);
        return rawImage;
    }

    UnityEngine::Sprite* FileToSprite(const std::string_view& filePath) {
        std::ifstream instream(filePath, std::ios::in | std::ios::binary | std::ios::ate);
        il2cpp_array_size_t size = instream.tellg();
        instream.seekg(0, instream.beg);
        ArrayW<uint8_t> bytes(size);
        instream.read(reinterpret_cast<char*>(bytes->_values), size);
        return ArrayToSprite(bytes);
    }

    UnityEngine::Sprite* TextureToSprite(UnityEngine::Texture2D* tex) {
        return (tex && tex->m_CachedPtr.m_value) ? UnityEngine::Sprite::Create(tex, UnityEngine::Rect(0.0f, 0.0f, (float)tex->get_width(), (float)tex->get_height()), UnityEngine::Vector2(0.5f,0.5f), 100.0f, 1u, UnityEngine::SpriteMeshType::FullRect, UnityEngine::Vector4(0.0f, 0.0f, 0.0f, 0.0f), false) : nullptr;
    }

    UnityEngine::Sprite* Base64ToSprite(const std::string_view& base64Str) {
        using base64 = cppcodec::base64_rfc4648;

        ArrayW<uint8_t> bytes(base64::decoded_max_size(base64Str.size()));
        base64::decode(bytes->begin(), bytes.size(), base64Str.data(), base64Str.size());
        return ArrayToSprite(bytes);
    }

    UnityEngine::Sprite* VectorToSprite(const std::vector<uint8_t>& bytes) {
        ArrayW<uint8_t> arr(il2cpp_array_size_t(bytes.size()));
        std::memcpy(arr->_values, bytes.data(), bytes.size());
        return ArrayToSprite(arr);
    }

    UnityEngine::Sprite* ArrayToSprite(ArrayW<uint8_t> bytes) {
        UnityEngine::Texture2D* texture = UnityEngine::Texture2D::New_ctor(0, 0, UnityEngine::TextureFormat::RGBA32, false, false);
        if (UnityEngine::ImageConversion::LoadImage(texture, bytes, false)) {
            return TextureToSprite(texture);
        }
        UnityEngine::Object::DestroyImmediate(texture);
        return nullptr;
    }

}
