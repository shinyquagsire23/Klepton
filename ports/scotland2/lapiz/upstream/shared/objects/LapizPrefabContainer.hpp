#pragma once

#include "custom-types/shared/macros.hpp"
#include "macros.hpp"

#include "UnityEngine/MonoBehaviour.hpp"
#include "UnityEngine/GameObject.hpp"
#include "Zenject/MonoMemoryPool_1.hpp"

DECLARE_CLASS_CODEGEN(Lapiz::Objects, LapizPrefabContainer, UnityEngine::MonoBehaviour) {
    DECLARE_PRIVATE_SERIALIZABLE_FIELD(UnityEngine::GameObject*, _prefab);
    DECLARE_INSTANCE_METHOD(UnityEngine::GameObject*, get_prefab);
    DECLARE_INSTANCE_METHOD(void, set_prefab, UnityEngine::GameObject* value);

    DECLARE_INSTANCE_METHOD(void, Start);
};