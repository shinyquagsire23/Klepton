#include "utilities/ZenjectExtensions.hpp"
#include "utilities/typeutil.hpp"
#include "utilities/logging.hpp"

#include "VRUIControls/VRGraphicRaycaster.hpp"
#include "VRUIControls/PhysicsRaycasterWithCache.hpp"
#include "HMUI/ViewController.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Canvas.hpp"
#include "UnityEngine/RectTransform.hpp"
#include "UnityEngine/Vector2.hpp"
#include "UnityEngine/Vector3.hpp"
#include "UnityEngine/AdditionalCanvasShaderChannels.hpp"
#include "Zenject/DiContainer.hpp"
#include "Zenject/BindInfo.hpp"
#include "Zenject/ScopableBindingFinalizer.hpp"
#include "Zenject/AddToNewGameObjectComponentProvider.hpp"
#include "Zenject/IProvider.hpp"

#include "custom-types/shared/delegate.hpp"
#include "System/Action_2.hpp"
#include "System/Func_3.hpp"
#include "System/Type.hpp"

DEFINE_TYPE(Lapiz::Zenject::ZenjectExtensions, DummyRaycaster);
using namespace UnityEngine;
using namespace VRUIControls;

static inline AdditionalCanvasShaderChannels operator |(AdditionalCanvasShaderChannels a, AdditionalCanvasShaderChannels b) {
    return a.value__ | b.value__;
}

namespace Lapiz::Zenject::ZenjectExtensions {
    ::Zenject::ScopeConcreteIdArgConditionCopyNonLazyBinder* FromNewComponentAsViewController(::Zenject::FromBinder* binder, std::function<void(::Zenject::InjectContext*, System::Object*)> onInstantiated) {
        auto go = GameObject::New_ctor("ViewController");

        go->SetActive(false);
        auto canvas = go->AddComponent<Canvas*>();
        canvas->set_additionalShaderChannels(AdditionalCanvasShaderChannels::Normal | AdditionalCanvasShaderChannels::TexCoord1 | AdditionalCanvasShaderChannels::TexCoord2 | AdditionalCanvasShaderChannels::Tangent);

        auto raycaster = go->AddComponent<DummyRaycaster*>();
        auto componentBinding = binder->FromNewComponentOn(go);
        raycaster->set_enabled(false);

        auto delegate = custom_types::MakeDelegate<System::Action_2<::Zenject::InjectContext*, System::Object*>*>(
            std::function<void(::Zenject::InjectContext*, System::Object*)>(
                [onInstantiated, raycaster, go](::Zenject::InjectContext* ctx, System::Object* obj){
                    auto vc = il2cpp_utils::try_cast<HMUI::ViewController>(obj).value_or(nullptr);
                    if (vc && vc->m_CachedPtr.m_value)
                    {
                        Object::DestroyImmediate(raycaster);
                        auto newRaycaster = go->AddComponent<VRGraphicRaycaster*>();
                        auto cache = ctx->get_Container()->Resolve<PhysicsRaycasterWithCache*>();
                        newRaycaster->_physicsRaycaster = cache;
                        vc->_graphicRaycaster = newRaycaster;

                        go->set_name(vc->GetType()->get_Name());
                        auto rt = vc->get_rectTransform();
                        rt->set_localEulerAngles({0, 0, 0});
                        rt->set_anchorMax({1, 1});
                        rt->set_localScale({1, 1, 1});
                        rt->set_anchorMin({0, 0});
                        rt->set_sizeDelta({0, 0});
                    }
                    if (onInstantiated) onInstantiated(ctx, obj);
                }
            )
        );

        componentBinding->OnInstantiated(delegate);
        return componentBinding;
    }

    ::Zenject::NameTransformScopeConcreteIdArgConditionCopyNonLazyBinder* FromNewComponentOnNewGameObject(::Zenject::FromBinder* fromBinder, ::Zenject::GameObjectCreationParameters* gameObjectInfo) {
        ListW<System::Type*> concreteTypes{fromBinder->get_ConcreteTypes()};
        for (int i = 0; auto type : concreteTypes) {
            static auto componentKlass = classof(UnityEngine::Component*);
            auto klass = il2cpp_functions::class_from_system_type(reinterpret_cast<Il2CppReflectionType*>(type));
            if (!TypeUtil::hasAncestor(klass, componentKlass)) {
                throw std::runtime_error(fmt::format("Invalid type given during bind command.  Expected type '{}' to derive from UnityEngine.Component", klass->name));
            }
            if (type->get_IsAbstract()) {
                throw std::runtime_error(fmt::format("Invalid type given during bind command.  Expected type '{}' to not be abstract.", klass->name));
            }
        }

        fromBinder->get_BindInfo()->RequireExplicitScope = true;

        auto delegate = custom_types::MakeDelegate<System::Func_3<::Zenject::DiContainer*, ::System::Type*, ::Zenject::IProvider*>*>(
            std::function<::Zenject::IProvider*(::Zenject::DiContainer*, ::System::Type*)>(
                [fromBinder, gameObjectInfo](::Zenject::DiContainer* container, ::System::Type* type) -> ::Zenject::IProvider* {
                    return *::Zenject::AddToNewGameObjectComponentProvider::New_ctor(
                        container, type, reinterpret_cast<System::Collections::Generic::IEnumerable_1<::Zenject::TypeValuePair>*>(fromBinder->get_BindInfo()->Arguments), gameObjectInfo, fromBinder->get_BindInfo()->ConcreteIdentifier, fromBinder->get_BindInfo()->InstantiatedCallback
                    );
                }
            )
        );

        auto finalizer = ::Zenject::ScopableBindingFinalizer::New_ctor(fromBinder->get_BindInfo(), delegate);

        fromBinder->set_SubFinalizer(*finalizer);

        return ::Zenject::NameTransformScopeConcreteIdArgConditionCopyNonLazyBinder::New_ctor(fromBinder->get_BindInfo(), gameObjectInfo);
    }

    UnityEngine::Camera* DummyRaycaster::get_eventCamera() {
        return UnityEngine::Camera::get_main();
    }

    void DummyRaycaster::Raycast(UnityEngine::EventSystems::PointerEventData* pointerEventData, ListW<UnityEngine::EventSystems::RaycastResult> resultAppendList) {

    }
}
