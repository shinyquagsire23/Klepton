#pragma once

#include "custom-types/shared/macros.hpp"
#include "macros.hpp"
#include "Zenject/DiContainer.hpp"
#include "System/IDisposable.hpp"

DECLARE_CLASS_CODEGEN_INTERFACES(Lapiz::Objects, ContractRecycler, System::Object, System::IDisposable*) {
    DECLARE_INSTANCE_FIELD_PRIVATE(::Zenject::DiContainer*, _container);
    DECLARE_CTOR(ctor, ::Zenject::DiContainer* container);
    DECLARE_OVERRIDE_METHOD_MATCH(void, Dispose, &::System::IDisposable::Dispose);
};