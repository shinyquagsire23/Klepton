#pragma once

#include "custom-types/shared/macros.hpp"
#include "UnityEngine/MonoBehaviour.hpp"

#include "GlobalNamespace/Saber.hpp"
#include "GlobalNamespace/NoteCutter.hpp"
#include "GlobalNamespace/ColorManager.hpp"
#include "GlobalNamespace/SaberTypeObject.hpp"
#include "GlobalNamespace/SaberModelController.hpp"
#include "GlobalNamespace/SaberMovementData.hpp"
#include "SaberModelProvider.hpp"

#include "../macros.hpp"
#include <queue>

DECLARE_CLASS_CODEGEN(Lapiz::Sabers, LapizSaber, UnityEngine::MonoBehaviour) {
    DECLARE_INSTANCE_FIELD(GlobalNamespace::NoteCutter*, _noteCutter);
    DECLARE_INSTANCE_FIELD(GlobalNamespace::ColorManager*, _colorManager);
    DECLARE_INSTANCE_FIELD(SaberModelProvider*, _saberModelProvider);
    DECLARE_INSTANCE_FIELD(GlobalNamespace::Saber*, _saber);
    DECLARE_INSTANCE_FIELD(GlobalNamespace::SaberTypeObject*, _saberTypeObject);
    DECLARE_INSTANCE_FIELD(GlobalNamespace::SaberModelController*, _saberModelController);
    DECLARE_INSTANCE_FIELD(GlobalNamespace::SaberMovementData*, _trailSaberMovementData);
    DECLARE_INSTANCE_FIELD(bool, _constructedThisFrame);
    
    DECLARE_INSTANCE_METHOD(void, Update);
    DECLARE_INSTANCE_METHOD(void, LateUpdate);
    DECLARE_INJECT_METHOD(void, Construct, GlobalNamespace::NoteCutter* noteCutter, GlobalNamespace::ColorManager* colorManager, SaberModelProvider* saberModelProvider);

    public:
        template<typename T>
        requires(std::is_convertible_v<T, GlobalNamespace::Saber*>)
        void Setup(GlobalNamespace::SaberType saberType) { Setup(csTypeOf(T), saberType); }
        void SetType(GlobalNamespace::SaberType saberType);
        void SetColor(UnityEngine::Color color);
        DECLARE_DEFAULT_CTOR();
    private:
        friend class LapizSaberFactory;
        std::function<void(GlobalNamespace::Saber*, UnityEngine::Color)> colorUpdated;
        std::queue<std::function<void(void)>> _colorProcessNextFrame;
        void Setup(System::Type* backingType, GlobalNamespace::SaberType saberType);
};