#pragma once

#include "custom-types/shared/macros.hpp"
#include "UnityEngine/MonoBehaviour.hpp"
#include <string>

DECLARE_CLASS_CODEGEN(BSML, HotReloadFileWatcher, UnityEngine::MonoBehaviour) {
    DECLARE_INSTANCE_FIELD(System::Object*, host);
    DECLARE_INSTANCE_FIELD(bool, runCheck);
    DECLARE_INSTANCE_FIELD(int, lastFileEdit);
    DECLARE_INSTANCE_FIELD(int, fileHash);
    DECLARE_INSTANCE_FIELD(float, timeSinceLastCheck);
    DECLARE_INSTANCE_FIELD(float, checkInterval);

    DECLARE_INSTANCE_METHOD(void, Update);
    DECLARE_INSTANCE_METHOD(void, Reload);

    DECLARE_CTOR(ctor);

    public:
        std::string filePath;
};