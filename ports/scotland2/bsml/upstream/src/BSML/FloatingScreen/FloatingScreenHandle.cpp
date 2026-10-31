#include "BSML/FloatingScreen/FloatingScreenHandle.hpp"

#include "UnityEngine/Shader.hpp"
#include "UnityEngine/Vector3.hpp"

#include "UnityEngine/EventSystems/EventSystem.hpp"
#include "VRUIControls/VRInputModule.hpp"
#include "UnityEngine/Quaternion.hpp"
#include "UnityEngine/Resources.hpp"
#include "BSML/FloatingScreen/FloatingScreen.hpp"

#include "logging.hpp"
#include "UnityEngine/Time.hpp"

DEFINE_TYPE(BSML, FloatingScreenHandle);

using namespace UnityEngine;

static UnityEngine::Quaternion operator*(UnityEngine::Quaternion a, UnityEngine::Quaternion b) {
    return UnityEngine::Quaternion::op_Multiply(a, b);
}

static bool operator==(UnityEngine::Quaternion a, UnityEngine::Quaternion b) {
    return UnityEngine::Quaternion::op_Equality(a, b);
}

static Vector3 operator-(Vector3 a, Vector3 b) {
    return Vector3::op_Subtraction(a,b);
}

static Vector3 operator+(Vector3 a, Vector3 b) {
    return Vector3::op_Addition(a,b);
}

static Vector3 operator*(Vector3 a, float_t b) {
    return Vector3::op_Multiply(a,b);
}

static Vector3 operator*(float_t a, Vector3 b) {
    return Vector3::op_Multiply(a,b);
}



namespace BSML {
    SafePtrUnity<UnityEngine::Shader> FloatingScreenHandle::shader;
    int FloatingScreenHandle::ColorId = 0;

    void FloatingScreenHandle::Awake() {
        if (!shader) {
            using namespace std::string_view_literals;
            shader = UnityEngine::Resources::FindObjectsOfTypeAll<UnityEngine::Shader*>()->First([](auto x){ return x->name == "Custom/Glowing"sv; });
            if (!shader) {
                ERROR("Failed to find shader for FloatingScreenHandle!");
            }

            ColorId = UnityEngine::Shader::PropertyToID("_Color");
        }

        _renderer = GetComponent<UnityEngine::MeshRenderer*>();
        _material = _renderer->get_material();
        _material->set_shader(shader.ptr());
        _material->SetColor(ColorId, DefaultColor);
    }

    void FloatingScreenHandle::OnPointerEnter(UnityEngine::EventSystems::PointerEventData* eventData) {
        _isHovering = true;
        UpdateMaterial();
    }

    void FloatingScreenHandle::OnPointerExit(UnityEngine::EventSystems::PointerEventData* eventData) {
        _isHovering = false;
        UpdateMaterial();
    }

    void FloatingScreenHandle::OnPointerUp(UnityEngine::EventSystems::PointerEventData* eventData) {
        auto currentEventSystem = EventSystems::EventSystem::get_current();
        if (!currentEventSystem) return;
        auto vrInputModule = currentEventSystem->get_currentInputModule().try_cast<VRUIControls::VRInputModule>().value_or(nullptr);
        if (!vrInputModule) return;

        _grabbingController = nullptr;
        auto pointer = vrInputModule->_vrPointer;
        _floatingScreen->OnHandleReleased(pointer);

        UpdateMaterial();
    }

    void FloatingScreenHandle::OnPointerDown(UnityEngine::EventSystems::PointerEventData* eventData) {
        auto raycast = eventData->pointerPressRaycast;

        auto go = raycast.get_gameObject();
        if (go != this->get_gameObject()) return;

        auto currentEventSystem = EventSystems::EventSystem::get_current();
        if (!currentEventSystem) return;
        auto vrInputModule = currentEventSystem->get_currentInputModule().try_cast<VRUIControls::VRInputModule>().value_or(nullptr);
        if (!vrInputModule) return;

        auto vrPointer = vrInputModule->_vrPointer;
        auto vrController = vrPointer->_lastSelectedVrController;
        if (!vrController) return;

        _grabbingController = vrController;
        _grabPos = vrController->_viewAnchorTransform->InverseTransformPoint(
                _floatingScreen->get_transform()->get_position()
        );
        _grabRot = Quaternion::Inverse(vrController->_viewAnchorTransform->get_rotation()) * _floatingScreen->get_transform()->get_rotation();

        _floatingScreen->OnHandleGrab(vrPointer);

        UpdateMaterial();
    }

    void FloatingScreenHandle::Init(FloatingScreen* floatingScreen) {
        _floatingScreen = floatingScreen;
    }

    void FloatingScreenHandle::Update() {
        // If we're not grabbing, don't do anything
        if (!_grabbingController) return;

        _grabPos -= Vector3::get_forward() * (-_grabbingController->get_thumbstick().y * Time::get_unscaledDeltaTime());
        Vector3 targetPosition = _grabbingController->_viewAnchorTransform->get_transform()->TransformPoint(_grabPos);
        Quaternion targetRotation = _grabbingController->_viewAnchorTransform->get_transform()->get_rotation() * _grabRot;

        _floatingScreen->get_transform()->SetPositionAndRotation(
            Vector3::Lerp(_floatingScreen->get_transform()->get_position(), targetPosition, 10.0f * Time::get_unscaledDeltaTime()),
            Quaternion::Lerp(_floatingScreen->get_transform()->get_rotation(), targetRotation, 5.0f * Time::get_unscaledDeltaTime())
        );
    }

    void FloatingScreenHandle::OnDestroy() {
        _grabbingController = nullptr;
        _floatingScreen = nullptr;
    }

    void FloatingScreenHandle::UpdateMaterial() {
        if (_floatingScreen->_highlightHandle && (_isHovering || _grabbingController)) {
            _material->SetColor(ColorId, HoverColor);
        } else {
            _material->SetColor(ColorId, DefaultColor);
        }
    }
}
