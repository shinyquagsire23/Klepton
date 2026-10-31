#pragma once

#include "custom-types/shared/macros.hpp" 
#include "macros.hpp" 
#include "GlobalNamespace/SaberClashChecker.hpp"
#include "GlobalNamespace/SaberManager.hpp"
#include "GlobalNamespace/Saber.hpp"

#include "Zenject/DiContainer.hpp"
#include "sabers/LapizSaberFactory.hpp"

DECLARE_CLASS_CODEGEN_INTERFACES(Lapiz::Sabers::Effects, LapizSaberClashChecker, GlobalNamespace::SaberClashChecker, System::IDisposable*) {
    DECLARE_INSTANCE_FIELD_PRIVATE(GlobalNamespace::Saber*, _lastSaberA);
    DECLARE_INSTANCE_FIELD_PRIVATE(GlobalNamespace::Saber*, _lastSaberB);
    DECLARE_INSTANCE_FIELD_PRIVATE(bool, _extraSabersDetected);
    DECLARE_INSTANCE_FIELD_PRIVATE_DEFAULT(ListW<GlobalNamespace::Saber*>, _sabers, nullptr);
    DECLARE_INSTANCE_FIELD_PRIVATE(::Zenject::DiContainer*, _container);
    DECLARE_INSTANCE_FIELD_PRIVATE(GlobalNamespace::SaberManager*, _saberManager);
    DECLARE_INSTANCE_FIELD_PRIVATE(Lapiz::Sabers::LapizSaberFactory*, _lapizSaberFactory);
    
    DECLARE_INSTANCE_METHOD(void, LapizSaberFactory_SaberCreated, Lapiz::Sabers::LapizSaber* lapizSaber);
    DECLARE_OVERRIDE_METHOD_MATCH(void, Dispose, &::System::IDisposable::Dispose);
    DECLARE_CTOR(ctor, ::Zenject::DiContainer* container, GlobalNamespace::SaberManager* saberManager, Lapiz::Sabers::LapizSaberFactory* lapizSaberFactory);
    public:
        bool SaberClashChecker_AreSabersClashing_override(UnityEngine::Vector3& clashingPoint, bool& result);
        UnorderedEventCallback<GlobalNamespace::Saber*, GlobalNamespace::Saber*> NewSabersClashed;
};