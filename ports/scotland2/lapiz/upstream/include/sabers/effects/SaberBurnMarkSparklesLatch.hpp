#pragma once

#include "custom-types/shared/macros.hpp"
#include "macros.hpp"

#include "System/IDisposable.hpp"
#include "sabers/LapizSaberFactory.hpp"
#include "sabers/SaberModelManager.hpp"

#include "GlobalNamespace/SaberBurnMarkSparkles.hpp"

#include "System/Collections/Generic/Queue_1.hpp"

DECLARE_CLASS_CODEGEN_INTERFACES(Lapiz::Sabers::Effects, SaberBurnMarkSparklesLatch, System::Object, System::IDisposable*) {
    DECLARE_INSTANCE_FIELD_PRIVATE(Lapiz::Sabers::LapizSaberFactory*, _lapizSaberFactory);
    DECLARE_INSTANCE_FIELD_PRIVATE(Lapiz::Sabers::SaberModelManager*, _saberModelManager);
    using EarlySabers = System::Collections::Generic::Queue_1<Lapiz::Sabers::LapizSaber*>;
    DECLARE_INSTANCE_FIELD_PRIVATE(EarlySabers*, _earlySabers);
    DECLARE_INSTANCE_FIELD_PRIVATE(GlobalNamespace::SaberBurnMarkSparkles*, _saberBurnMarkSparkles);
    DECLARE_INSTANCE_FIELD_PRIVATE(bool, _sisterLoopActive);
    DECLARE_INSTANCE_FIELD_PRIVATE(int, _activeSaberIndex);

    DECLARE_OVERRIDE_METHOD_MATCH(void, Dispose, &::System::IDisposable::Dispose);
    DECLARE_INSTANCE_METHOD(void, LapizSaberFactory_SaberCreated, Lapiz::Sabers::LapizSaber* lapizSaber);
    DECLARE_PRIVATE_METHOD(void, AddSaber, GlobalNamespace::Saber* saber);
    DECLARE_PRIVATE_METHOD(UnityEngine::ParticleSystem*, CreateNewBurnMarkParticles);

    DECLARE_CTOR(ctor, Lapiz::Sabers::SaberModelManager* saberModelManager, Lapiz::Sabers::LapizSaberFactory* lapizSaberFactory);
    public:
        void SaberBurnMarkSparkles_Start_Postfix(GlobalNamespace::SaberBurnMarkSparkles* self);
        void SaberBurnMarkSparkles_LateUpdate_Prefix(GlobalNamespace::SaberBurnMarkSparkles* self);
        void SaberBurnMarkSparkles_LateUpdate_Postfix(GlobalNamespace::SaberBurnMarkSparkles* self);
        bool ColorManager_ColorForSaberType_Prefix(UnityEngine::Color& result);
        static SaberBurnMarkSparklesLatch* get_instance();
    private:
        static SaberBurnMarkSparklesLatch* instance;
};