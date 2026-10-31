#pragma once

#include "custom-types/shared/macros.hpp"
#include "Zenject/ILateTickable.hpp"
#include "System/IDisposable.hpp"

#include "GlobalNamespace/ColorManager.hpp"
#include "LapizSaberFactory.hpp"

#include "System/Collections/Generic/Dictionary_2.hpp"
#include "../macros.hpp"

namespace Lapiz::Sabers { class SaberModelManager; }

DECLARE_CLASS_CODEGEN(Lapiz::Sabers, DesperationContract, System::Object) {
    DECLARE_INSTANCE_FIELD_PRIVATE(GlobalNamespace::Saber*, _saber);
    DECLARE_INSTANCE_FIELD_PRIVATE(UnityEngine::Color, _color);
    DECLARE_INSTANCE_FIELD_PRIVATE(int, _accessed);
    DECLARE_INSTANCE_METHOD(GlobalNamespace::Saber*, get_saber);

    DECLARE_CTOR(ctor, GlobalNamespace::Saber* saber, UnityEngine::Color color);
    static constexpr const int _maxAccessTimes = 10;
    friend class SaberModelManager;
};

DECLARE_CLASS_CODEGEN_INTERFACES(Lapiz::Sabers, SaberModelManager, System::Object, ::Zenject::ILateTickable*, ::System::IDisposable*) {
    DECLARE_INSTANCE_FIELD(GlobalNamespace::ColorManager*, _colorManager);
    DECLARE_INSTANCE_FIELD(LapizSaberFactory*, _lapizSaberFactory);

    using LapizSaberLink = System::Collections::Generic::Dictionary_2<GlobalNamespace::Saber*, LapizSaber*>;
    using SaberModelLink = System::Collections::Generic::Dictionary_2<GlobalNamespace::Saber*, GlobalNamespace::SaberModelController*>;
    DECLARE_INSTANCE_FIELD(LapizSaberLink*, _lapizSaberLink);
    DECLARE_INSTANCE_FIELD(SaberModelLink*, _saberModelLink);
    DECLARE_INSTANCE_FIELD_DEFAULT(ListW<DesperationContract*>, _desperationList, nullptr);
    DECLARE_INSTANCE_FIELD_DEFAULT(ListW<DesperationContract*>, _salvationList, nullptr);

    DECLARE_CTOR(ctor, GlobalNamespace::ColorManager* colorManager, LapizSaberFactory* lapizSaberFactory);
    DECLARE_OVERRIDE_METHOD_MATCH(void, LateTick, &::Zenject::ILateTickable::LateTick);
    DECLARE_OVERRIDE_METHOD_MATCH(void, Dispose, &::System::IDisposable::Dispose);

    DECLARE_INSTANCE_METHOD(GlobalNamespace::SaberModelController*, GetSaberModelController, GlobalNamespace::Saber* saber);
    DECLARE_INSTANCE_METHOD(UnityEngine::Color, GetPhysicalSaberColor, GlobalNamespace::Saber* saber);
    DECLARE_INSTANCE_METHOD(void, SetColor, GlobalNamespace::Saber* saber, UnityEngine::Color color);
    DECLARE_PRIVATE_METHOD(void, SiraSaberFactory_SaberCreated, LapizSaber* lapizSaber);
    public:
        std::queue<std::function<void()>> _colorUpdateQueue;
        UnorderedEventCallback<GlobalNamespace::Saber*, UnityEngine::Color> ColorUpdated;
};