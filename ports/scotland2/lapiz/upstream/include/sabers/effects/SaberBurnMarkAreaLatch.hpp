#pragma once

#include "custom-types/shared/macros.hpp"
#include "macros.hpp"

#include "System/IDisposable.hpp"
#include "sabers/LapizSaberFactory.hpp"
#include "sabers/SaberModelManager.hpp"

#include "GlobalNamespace/ColorManager.hpp"
#include "GlobalNamespace/SaberBurnMarkArea.hpp"
#include "UnityEngine/LineRenderer.hpp"
#include "UnityEngine/RenderTexture.hpp"

#include "System/Collections/Generic/Queue_1.hpp"

DECLARE_CLASS_CODEGEN_INTERFACES(Lapiz::Sabers::Effects, SaberBurnMarkAreaLatch, System::Object, System::IDisposable*) {
    DECLARE_INSTANCE_FIELD_PRIVATE(Lapiz::Sabers::LapizSaberFactory*, _lapizSaberFactory);
    DECLARE_INSTANCE_FIELD_PRIVATE(Lapiz::Sabers::SaberModelManager*, _saberModelManager);
    using EarlySabers = System::Collections::Generic::Queue_1<Lapiz::Sabers::LapizSaber*>;
    DECLARE_INSTANCE_FIELD_PRIVATE(EarlySabers*, _earlySabers);
    DECLARE_INSTANCE_FIELD_PRIVATE(GlobalNamespace::SaberBurnMarkArea*, _saberBurnMarkArea);
    DECLARE_INSTANCE_FIELD_PRIVATE(int, _lineFactoryIncrement);

    DECLARE_OVERRIDE_METHOD_MATCH(void, Dispose, &::System::IDisposable::Dispose);
    DECLARE_INSTANCE_METHOD(void, LapizSaberFactory_SaberCreated, Lapiz::Sabers::LapizSaber* lapizSaber);
    DECLARE_INSTANCE_METHOD(void, ColorUpdated, GlobalNamespace::Saber* saber, UnityEngine::Color color);
    DECLARE_PRIVATE_METHOD(void, AddSaber, GlobalNamespace::Saber* saber);

    DECLARE_PRIVATE_METHOD(UnityEngine::LineRenderer*, CreateNewLineRenderer, UnityEngine::Color initialColor);
    DECLARE_PRIVATE_METHOD(UnityEngine::RenderTexture*, CreateNewRenderTexture);

    DECLARE_CTOR(ctor, Lapiz::Sabers::SaberModelManager* saberModelManager, Lapiz::Sabers::LapizSaberFactory* lapizSaberFactory);
    public:
        void SaberBurnMarkArea_Start_Postfix(GlobalNamespace::SaberBurnMarkArea* self);
        void SaberBurnMarkArea_LateUpdate_Postfix(GlobalNamespace::SaberBurnMarkArea* self);
        static SaberBurnMarkAreaLatch* get_instance();
    private:
        static SaberBurnMarkAreaLatch* instance;
};