#pragma once

#include "custom-types/shared/macros.hpp"
#include "System/Object.hpp"
#include "Zenject/IInitializable.hpp"
#include "System/IDisposable.hpp"
#include "Zenject/DiContainer.hpp"

DECLARE_CLASS_CODEGEN_INTERFACES(SongCore::Overrides, RotationSpawnLinesOverride, System::Object, System::IDisposable*) {
    DECLARE_CTOR(ctor, Zenject::DiContainer* container);

    DECLARE_OVERRIDE_METHOD_MATCH(void, Dispose, &System::IDisposable::Dispose);
};