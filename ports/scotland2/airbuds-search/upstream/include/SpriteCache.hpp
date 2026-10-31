#pragma once

#include "UnityEngine/Sprite.hpp"

namespace AirbudsSearch {

class SpriteCache {

    public:

    UnityW<UnityEngine::Sprite> get(const std::string_view key);

    void add(const std::string_view key, UnityW<UnityEngine::Sprite> sprite);
    void addToDiskCache(const std::string& key, const std::vector<uint8_t>& data);

    static SpriteCache& getInstance() {
        static SpriteCache spriteCache;
        return spriteCache;
    }

    private:

    std::string getKeyHash(const std::string_view key);

    std::unordered_map<std::string, UnityW<UnityEngine::Sprite>> memoryCache_;

};

}
