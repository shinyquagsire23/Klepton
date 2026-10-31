#pragma once

#include "custom-types/shared/macros.hpp"
#include "macros.hpp"

#include "LapizSaberClashChecker.hpp"
#include "sabers/SaberModelManager.hpp"
#include "GlobalNamespace/SaberClashEffect.hpp"

#include "Zenject/IInitializable.hpp"
#include "System/IDisposable.hpp"

DECLARE_CLASS_CODEGEN_INTERFACES(Lapiz::Sabers::Effects, SaberClashEffectAdjuster, System::Object, ::Zenject::IInitializable*, System::IDisposable*) {
    DECLARE_INSTANCE_FIELD_PRIVATE(GlobalNamespace::SaberClashEffect*, _saberClashEffect);
    DECLARE_INSTANCE_FIELD_PRIVATE(UnityEngine::ParticleSystem*, _glowParticleSystem);
    DECLARE_INSTANCE_FIELD_PRIVATE(UnityEngine::ParticleSystem*, _sparkleParticleSystem);
    DECLARE_INSTANCE_FIELD_PRIVATE(Lapiz::Sabers::SaberModelManager*, _saberModelManager);
    DECLARE_INSTANCE_FIELD_PRIVATE(Lapiz::Sabers::Effects::LapizSaberClashChecker*, _saberClashChecker);

    DECLARE_OVERRIDE_METHOD_MATCH(void, Initialize, &::Zenject::IInitializable::Initialize);
    DECLARE_OVERRIDE_METHOD_MATCH(void, Dispose, &::System::IDisposable::Dispose);
    DECLARE_CTOR(ctor, Lapiz::Sabers::SaberModelManager* saberModelManager, GlobalNamespace::SaberClashChecker* saberClashChecker);
    DECLARE_PRIVATE_METHOD(void, SaberClashChecker_NewSabersClashed, GlobalNamespace::Saber* saberA, GlobalNamespace::Saber* saberB);
    public:
        void SaberClashEffect_Start_Postfix(GlobalNamespace::SaberClashEffect* self);
        static SaberClashEffectAdjuster* get_instance();
    private:
        static SaberClashEffectAdjuster* instance;
};