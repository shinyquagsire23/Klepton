#pragma once

#include "UnityEngine/Resources.hpp"

class ResettableStaticPtr {
    static std::unordered_map<void*, void*>& pointers() {
        static std::unordered_map<void*, void*> pointers = {};
        return pointers;
    }

   public:
    template <class T>
    static T*& registerPointer(T* ptr) {
        auto pair = pointers().emplace(ptr, ptr);
        return (T*&) pair.first->second;
    }

    static void resetAll() {
        for (auto& pair : pointers()) {
            pair.second = nullptr;
        }
    }
};

#define STATIC_AUTO(name, definition) \
static auto& name = ResettableStaticPtr::registerPointer(definition); \
if(!name) name = definition;

template <class T>
T FindComponent() {
    STATIC_AUTO(cachedComponent, UnityEngine::Resources::FindObjectsOfTypeAll<T>()->FirstOrDefault());
    if (!cachedComponent)
        SAFE_ABORT();
    return cachedComponent;
}
