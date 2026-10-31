#include "bsml/shared/BSML-Lite/Creation/Image.hpp"

#include "SpriteCache.hpp"
#include "main.hpp"

using namespace AirbudsSearch;

bool isSpriteValid(UnityW<UnityEngine::Sprite> sprite) {
    if (!sprite) {
        return false;
    }

    if (!UnityEngine::Object::IsNativeObjectAlive(sprite)) {
        return false;
    }

    // Even if the sprite itself is valid, the underlying texture might have been destroyed
    const UnityW<UnityEngine::Texture2D> texture = sprite->get_texture();
    if (!texture) {
        return false;
    }
    if (!UnityEngine::Object::IsNativeObjectAlive(texture)) {
        return false;
    }

    return true;
}

UnityW<UnityEngine::Sprite> SpriteCache::get(const std::string_view key) {
    const std::string hashedKey = getKeyHash(key);

    // Check the memory cache
    auto iterator = memoryCache_.find(hashedKey);
    if (iterator != memoryCache_.end()) {
        const UnityW<UnityEngine::Sprite> sprite = iterator->second;
        if (isSpriteValid(sprite)) {
            return sprite;
        }
        // Sprite is dead, remove the cache entry
        memoryCache_.erase(hashedKey);
        AirbudsSearch::Log.info("Removing dead sprite from cache. key = {}", hashedKey);
    }

    // Check disk cache
    const std::filesystem::path path = AirbudsSearch::getDataDirectory() / "cache" / hashedKey;
    if (std::filesystem::exists(path)) {
        const UnityW<UnityEngine::Sprite> sprite = BSML::Lite::FileToSprite(path.string());
        if (sprite) {
            memoryCache_[hashedKey] = sprite;
            return sprite;
        }

        // The sprite was in the disk cache, but we failed to load it. The file is probably corrupted, so let's delete
        // it.
        AirbudsSearch::Log.warn("Failed to load sprite from disk cache: {}. Removing it...", path.string());
        std::filesystem::remove(path);
    }

    return nullptr;
}

void SpriteCache::add(const std::string_view key, UnityW<UnityEngine::Sprite> sprite) {
    const std::string hashedKey = getKeyHash(key);
    memoryCache_[hashedKey] = sprite;
}

void SpriteCache::addToDiskCache(const std::string& key, const std::vector<uint8_t>& data) {
    const std::string hashedKey = getKeyHash(key);
    const std::filesystem::path path = AirbudsSearch::getDataDirectory() / "cache" / hashedKey;
    if (!std::filesystem::exists(path)) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream outputFileStream(path, std::ios::binary);
        if (outputFileStream) {
            outputFileStream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
            outputFileStream.close();
        } else {
            AirbudsSearch::Log.warn("Failed to open file for writing: {}", path.string());
        }
    }
}

std::string SpriteCache::getKeyHash(const std::string_view key) {
    const std::size_t hash = std::hash<std::string_view>{}(key);
    return std::format("{:016x}", hash);
}
