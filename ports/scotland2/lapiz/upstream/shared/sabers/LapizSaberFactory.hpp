#pragma once

#include "custom-types/shared/macros.hpp"
#include "LapizSaber.hpp"

#include "../macros.hpp"

DECLARE_CLASS_CODEGEN(Lapiz::Sabers, LapizSaberFactory, System::Object) {
    DECLARE_INSTANCE_FIELD_PRIVATE(::Zenject::DiContainer*, _container);
    DECLARE_CTOR(ctor, ::Zenject::DiContainer* container);
    public:
        template<typename T = GlobalNamespace::Saber*>
        requires(std::is_convertible_v<T, GlobalNamespace::Saber*>)
        LapizSaber* Spawn(GlobalNamespace::SaberType saberType) {
            return Spawn(csTypeOf(T), saberType);
        }

        UnorderedEventCallback<LapizSaber*> SaberCreated;
        UnorderedEventCallback<GlobalNamespace::Saber*, UnityEngine::Color> ColorUpdated;
    private:
        void UpdateColorInternal(GlobalNamespace::Saber* saber, UnityEngine::Color color);
        LapizSaber* Spawn(System::Type* backingSaberType, GlobalNamespace::SaberType saberType);
};