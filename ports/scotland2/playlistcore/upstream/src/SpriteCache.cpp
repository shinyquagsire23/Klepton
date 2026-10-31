#include "SpriteCache.hpp"

#include <map>

#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/UI/Image.hpp"

using namespace UnityEngine;

std::unordered_map<Sprite*, GameObject*> caches;
std::map<std::string, Sprite*, std::less<>> strings;

void CacheSprite(Sprite* sprite, std::string base64) {
    if (!caches.contains(sprite)) {
        static ConstString name("PlaylistCoreCachedSprite");
        auto object = GameObject::New_ctor(name);
        object->AddComponent<UI::Image*>()->set_sprite(sprite);
        Object::DontDestroyOnLoad(object);
        caches.emplace(sprite, object);
        strings.emplace(std::move(base64), sprite);
    }
}

Sprite* HasCachedSprite(std::string_view base64) {
    auto findIter = strings.find(base64);
    if (findIter != strings.end())
        return findIter->second;
    return nullptr;
}

void RemoveCachedSprite(Sprite* sprite) {
    if (caches.contains(sprite)) {
        Object::Destroy(caches.find(sprite)->second);
        caches.erase(sprite);
        for (auto iter = strings.begin(); iter != strings.end(); iter++) {
            if (iter->second == sprite) {
                strings.erase(iter);
                break;
            }
        }
    }
}

void ClearCachedSprites() {
    for (auto& pair : caches) {
        if (pair.second && pair.second->m_CachedPtr.m_value) {
            Object::Destroy(pair.second);
        }
    }
    caches.clear();
    strings.clear();
}
