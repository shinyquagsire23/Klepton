#pragma once

#include "UnityEngine/Sprite.hpp"

void CacheSprite(UnityEngine::Sprite* sprite, std::string base64);

UnityEngine::Sprite* HasCachedSprite(std::string_view base64);

void RemoveCachedSprite(UnityEngine::Sprite* sprite);

void ClearCachedSprites();
