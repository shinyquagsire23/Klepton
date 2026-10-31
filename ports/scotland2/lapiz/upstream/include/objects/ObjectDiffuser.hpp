#pragma once

#include "custom-types/shared/macros.hpp"
#include "macros.hpp"
#include "UnityEngine/Object.hpp"

DECLARE_CLASS_CODEGEN(Lapiz::Objects, ObjectDiffuser, System::Object) {
    DECLARE_INSTANCE_FIELD_PRIVATE(UnityEngine::Object*, _object);
    DECLARE_INJECT_METHOD(void, Resolved);
    DECLARE_CTOR(ctor, UnityEngine::Object* obj);
};