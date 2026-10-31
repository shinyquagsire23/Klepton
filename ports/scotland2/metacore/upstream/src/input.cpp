#include "input.hpp"

#include "GlobalNamespace/OVRInput.hpp"
#include "UnityEngine/EventSystems/EventSystem.hpp"
#include "UnityEngine/Resources.hpp"
#include "UnityEngine/SpatialTracking/PoseDataSource.hpp"
#include "UnityEngine/XR/XRNode.hpp"
#include "unity.hpp"

using namespace GlobalNamespace;

static std::vector<OVRInput::Button> const ButtonsMap = {
    OVRInput::Button::One,
    OVRInput::Button::Two,
    OVRInput::Button::PrimaryThumbstick,
    OVRInput::Button::PrimaryHandTrigger,
    OVRInput::Button::PrimaryIndexTrigger,
    OVRInput::Button::Start,
};

static std::vector<OVRInput::Axis1D> const AxesMap = {
    OVRInput::Axis1D::None,
    OVRInput::Axis1D::None,
    OVRInput::Axis1D::PrimaryHandTrigger,
    OVRInput::Axis1D::PrimaryIndexTrigger,
};

static std::vector<OVRInput::Controller> const ControllersMap = {
    OVRInput::Controller::LTouch, OVRInput::Controller::RTouch, OVRInput::Controller::Active, OVRInput::Controller::Touch
};

static std::set<std::string> hapticsDisablers;

bool MetaCore::Input::GetPressed(Controllers controller, Buttons button) {
    if (button < 0 || button >= ButtonsMap.size() || controller < 0 || controller >= ControllersMap.size())
        return false;
    return OVRInput::Get(ButtonsMap[button], ControllersMap[controller]);
}

float MetaCore::Input::GetAxis(Controllers controller, Axes axis) {
    if (axis < 0 || axis >= AxesMap.size() || controller < 0 || controller >= ControllersMap.size())
        return 0;
    if (AxesMap[axis] != OVRInput::Axis1D::None)
        return OVRInput::Get(AxesMap[axis], ControllersMap[controller]);
    auto vector = GetThumbstick(controller);
    return axis == Axes::ThumbstickHorizontal ? vector.x : vector.y;
}

UnityEngine::Vector2 MetaCore::Input::GetThumbstick(Controllers controller) {
    return OVRInput::Get(OVRInput::Axis2D::PrimaryThumbstick, ControllersMap[controller]);
}

static UnityEngine::Pose GetNodePose(UnityEngine::XR::XRNode node) {
    auto pose = UnityEngine::Pose::get_identity();
    UnityEngine::SpatialTracking::PoseDataSource::GetNodePoseData(node, byref(pose));
    return pose;
}

UnityEngine::Pose MetaCore::Input::GetHandPose(bool left) {
    return GetNodePose(left ? UnityEngine::XR::XRNode::LeftHand : UnityEngine::XR::XRNode::RightHand);
}

UnityEngine::Pose MetaCore::Input::GetHeadPose() {
    return GetNodePose(UnityEngine::XR::XRNode::CenterEye);
}

static VRUIControls::VRInputModule* FindCurrentInputModule() {
    auto eventSystem = UnityEngine::EventSystems::EventSystem::get_current();
    if (!eventSystem) {
        auto eventSystems = UnityEngine::Resources::FindObjectsOfTypeAll<UnityEngine::EventSystems::EventSystem*>();
        eventSystem = eventSystems->FirstOrDefault([](auto e) { return e->isActiveAndEnabled; });
        if (!eventSystem)
            eventSystem = eventSystems->FirstOrDefault();
    }
    if (!eventSystem)
        return nullptr;
    return eventSystem->GetComponent<VRUIControls::VRInputModule*>();
}

VRUIControls::VRInputModule* MetaCore::Input::GetCurrentInputModule() {
    static VRUIControls::VRInputModule* input;
    if (!input) {
        input = FindCurrentInputModule();
        if (input)
            Engine::SetOnDisable(input, []() { input = nullptr; }, true);
    }
    return input;
}

void MetaCore::Input::SetHaptics(std::string mod, bool enable) {
    if (enable)
        hapticsDisablers.erase(mod);
    else
        hapticsDisablers.emplace(std::move(mod));
}

bool MetaCore::Input::IsHapticsDisabled() {
    return !hapticsDisablers.empty();
}

std::set<std::string> const& MetaCore::Input::GetHapticsDisablers() {
    return hapticsDisablers;
}
