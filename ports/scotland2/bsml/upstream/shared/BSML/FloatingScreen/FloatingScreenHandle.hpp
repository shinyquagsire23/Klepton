#pragma once

#include "custom-types/shared/macros.hpp"
#include "UnityEngine/MonoBehaviour.hpp"
#include "UnityEngine/Material.hpp"
#include "UnityEngine/MeshRenderer.hpp"
#include "UnityEngine/Color.hpp"
#include "GlobalNamespace/VRController.hpp"
#include "UnityEngine/Shader.hpp"
#include "VRUIControls/VRPointer.hpp"
#include "VRUIControls/VRInputModule.hpp"

#include "UnityEngine/EventSystems/IPointerEnterHandler.hpp"
#include "UnityEngine/EventSystems/IEventSystemHandler.hpp"
#include "UnityEngine/EventSystems/IPointerExitHandler.hpp"
#include "UnityEngine/EventSystems/IPointerDownHandler.hpp"
#include "UnityEngine/EventSystems/IPointerUpHandler.hpp"
#include "UnityEngine/EventSystems/PointerEventData.hpp"

namespace BSML {
    class FloatingScreen;
}

DECLARE_CLASS_CODEGEN_INTERFACES(BSML, FloatingScreenHandle, UnityEngine::MonoBehaviour, UnityEngine::EventSystems::IPointerEnterHandler*, UnityEngine::EventSystems::IPointerExitHandler*, UnityEngine::EventSystems::IEventSystemHandler*, UnityEngine::EventSystems::IPointerDownHandler*,UnityEngine::EventSystems::IPointerUpHandler*) {

    DECLARE_INSTANCE_FIELD(UnityW<GlobalNamespace::VRController>, _grabbingController);
    DECLARE_INSTANCE_FIELD(UnityW<UnityEngine::Material>, _material);
    DECLARE_INSTANCE_FIELD(UnityW<UnityEngine::MeshRenderer>, _renderer);
    DECLARE_INSTANCE_FIELD(UnityEngine::Vector3, _grabPos);
    DECLARE_INSTANCE_FIELD(UnityEngine::Quaternion, _grabRot);

    DECLARE_INSTANCE_METHOD(void, Awake);
    DECLARE_OVERRIDE_METHOD_MATCH(void, OnPointerEnter, &::UnityEngine::EventSystems::IPointerEnterHandler::OnPointerEnter, UnityEngine::EventSystems::PointerEventData* eventData);
    DECLARE_OVERRIDE_METHOD_MATCH(void, OnPointerExit, &::UnityEngine::EventSystems::IPointerExitHandler::OnPointerExit, UnityEngine::EventSystems::PointerEventData* eventData);
    DECLARE_OVERRIDE_METHOD_MATCH(void, OnPointerUp, &::UnityEngine::EventSystems::IPointerUpHandler::OnPointerUp, UnityEngine::EventSystems::PointerEventData* eventData);
    DECLARE_OVERRIDE_METHOD_MATCH(void, OnPointerDown, &::UnityEngine::EventSystems::IPointerDownHandler::OnPointerDown, UnityEngine::EventSystems::PointerEventData* eventData);

    DECLARE_INSTANCE_METHOD(void, UpdateMaterial);

    DECLARE_INSTANCE_METHOD(void, Update); 
    DECLARE_INSTANCE_METHOD(void, OnDestroy);

    public:
        void Init(FloatingScreen* floatingScreen);
    
    private:
        static SafePtrUnity<UnityEngine::Shader> shader;
        static int ColorId;
        bool _isHovering = false;
        static inline UnityEngine::Color DefaultColor = UnityEngine::Color(1.0f, 1.0f, 1.0f, 0.0f);
        static inline UnityEngine::Color HoverColor  = UnityEngine::Color(1.0f, 1.0f, 0.0f, 1.0f);
    protected:
        FloatingScreen* _floatingScreen;
};