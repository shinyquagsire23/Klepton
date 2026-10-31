#include "sabers/LapizSaberFactory.hpp"
#include "utilities/logging.hpp"

#include "UnityEngine/GameObject.hpp"

DEFINE_TYPE(Lapiz::Sabers, LapizSaberFactory);

namespace Lapiz::Sabers {
    void LapizSaberFactory::ctor(::Zenject::DiContainer* container) {
        INVOKE_CTOR();
        DEBUG("LapizSaberFactory ctor");
        _container = container;
    }

    void LapizSaberFactory::UpdateColorInternal(GlobalNamespace::Saber* saber, UnityEngine::Color color) {
        if (ColorUpdated.size() > 0) ColorUpdated.invoke(saber, color);
    }

    LapizSaber* LapizSaberFactory::Spawn(System::Type* backingSaberType, GlobalNamespace::SaberType saberType) {
        auto lapizSaberGameObject = UnityEngine::GameObject::New_ctor("Lapiz | LapizSaber");
        auto lapizSaber = _container->InstantiateComponent<LapizSaber*>(lapizSaberGameObject);
        lapizSaber->colorUpdated = std::bind(&LapizSaberFactory::UpdateColorInternal, this, std::placeholders::_1, std::placeholders::_2);
        lapizSaber->Setup(backingSaberType, saberType);
        if (SaberCreated.size() > 0) SaberCreated.invoke(lapizSaber);
        return lapizSaber;
    }
}