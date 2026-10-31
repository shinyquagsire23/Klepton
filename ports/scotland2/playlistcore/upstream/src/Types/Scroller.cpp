#include "Types/Scroller.hpp"

#include "GlobalNamespace/AnnotatedBeatmapLevelCollectionsGridViewAnimator.hpp"
#include "GlobalNamespace/VRController.hpp"
#include "HMUI/EventSystemListener.hpp"
#include "Main.hpp"
#include "ResettableStaticPtr.hpp"
#include "Types/Config.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Rect.hpp"
#include "UnityEngine/Time.hpp"
#include "metacore/shared/delegates.hpp"

// a scroller in multiple ways specialized for the playlist grid
// however, it could likely be made more versatile with a few changes
// of course, it would be nice if I could just use a regular ScrollView without it crashing...

DEFINE_TYPE(PlaylistCore, Scroller);

#define ACTION_1(type, methodname) \
    MetaCore::Delegates::MakeSystemAction([this](type arg) { if(this->cachedPtr == this) methodname(arg); })

using namespace PlaylistCore;

float fixedCellHeight = 15;

void Scroller::Awake() {
    platformHelper = FindComponent<GlobalNamespace::VRController*>()->_vrPlatformHelper;

    auto eventListener = GetComponent<HMUI::EventSystemListener*>();
    if (!eventListener)
        eventListener = get_gameObject()->AddComponent<HMUI::EventSystemListener*>();
    eventListener->add_pointerDidEnterEvent(ACTION_1(UnityEngine::EventSystems::PointerEventData*, HandlePointerDidEnter));
    eventListener->add_pointerDidExitEvent(ACTION_1(UnityEngine::EventSystems::PointerEventData*, HandlePointerDidExit));

    set_enabled(false);
}

void Scroller::Update() {
    if (!contentTransform || !platformHelper)
        return;
    if (platformHelper->hasInputFocus) {
        auto anyJoystickMaxAxis = platformHelper->GetAnyJoystickMaxAxis();
        if (anyJoystickMaxAxis.sqrMagnitude > 0.01)
            HandleJoystickWasNotCenteredThisFrame(anyJoystickMaxAxis);
    }
    auto pos = contentTransform->get_anchoredPosition();
    float newPos = std::lerp(pos.y, destinationPos, UnityEngine::Time::get_deltaTime() * 8);
    if (std::abs(newPos - destinationPos) < 0.01) {
        newPos = destinationPos;
        if (!pointerHovered)
            set_enabled(false);
    }
    contentTransform->set_anchoredPosition({pos.x, newPos});
}

void Scroller::OnDestroy() {
    // would be better to remove the delegate, I'll try that out when codegen updates to the bshook with it and better virtuals
    cachedPtr = nullptr;
}

void Scroller::Init(UnityEngine::RectTransform* content) {
    contentTransform = content;
    cachedPtr = this;
}

void Scroller::HandlePointerDidEnter(UnityEngine::EventSystems::PointerEventData* pointerEventData) {
    pointerHovered = true;
    float pos = FindComponent<GlobalNamespace::AnnotatedBeatmapLevelCollectionsGridViewAnimator*>()->GetContentYOffset();
    SetDestinationPos(pos);
}

void Scroller::HandlePointerDidExit(UnityEngine::EventSystems::PointerEventData* pointerEventData) {
    pointerHovered = false;
    set_enabled(false);
}

void Scroller::HandleJoystickWasNotCenteredThisFrame(UnityEngine::Vector2 deltaPos) {
    if (!pointerHovered)
        return;
    float num = destinationPos;
    num -= deltaPos.y * UnityEngine::Time::get_deltaTime() * 45 * getConfig().ScrollSpeed.GetValue();
    SetDestinationPos(num);
}

void Scroller::SetDestinationPos(float value) {
    if (!contentTransform)
        return;
    float contentSize = contentTransform->get_rect().get_height();
    float afterEndPageSize = fixedCellHeight * 4;
    float zeroPos = -(contentSize - fixedCellHeight) / 2;
    float endPos = -zeroPos - afterEndPageSize;
    float difference = contentSize - afterEndPageSize;
    if (difference <= 0)
        destinationPos = zeroPos;
    else
        destinationPos = std::clamp(value, zeroPos, endPos);
    set_enabled(true);
}
