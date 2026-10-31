#pragma once

#include "custom-types/shared/macros.hpp"

#include "HMUI/Screen.hpp"
#include "HMUI/ViewController.hpp"
#include "UnityEngine/Material.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Vector2.hpp"
#include "UnityEngine/Vector3.hpp"
#include "UnityEngine/Quaternion.hpp"
#include "UnityEngine/RectTransform.hpp"
#include "VRUIControls/VRPointer.hpp"
#include "FloatingScreenHandleEventArgs.hpp"
#include "Side.hpp"

DECLARE_CLASS_CODEGEN(BSML, FloatingScreen, HMUI::Screen) {
    DECLARE_INSTANCE_FIELD(UnityEngine::GameObject*, handle);

    DECLARE_INSTANCE_METHOD(UnityEngine::RectTransform*, get_rectTransform);
    DECLARE_INSTANCE_METHOD(void, OnHandleReleased, VRUIControls::VRPointer* pointer);
    DECLARE_INSTANCE_METHOD(void, OnHandleGrab, VRUIControls::VRPointer* pointer);
    
    DECLARE_INSTANCE_METHOD(UnityEngine::Vector2, get_ScreenSize);
    DECLARE_INSTANCE_METHOD(void, set_ScreenSize, UnityEngine::Vector2 value);

    DECLARE_INSTANCE_METHOD(UnityEngine::Vector3, get_ScreenPosition);
    DECLARE_INSTANCE_METHOD(void, set_ScreenPosition, UnityEngine::Vector3 value);

    DECLARE_INSTANCE_METHOD(UnityEngine::Quaternion, get_ScreenRotation);
    DECLARE_INSTANCE_METHOD(void, set_ScreenRotation, UnityEngine::Quaternion value);

    DECLARE_INSTANCE_FIELD(bool, _showHandle);
    DECLARE_INSTANCE_METHOD(bool, get_ShowHandle);
    DECLARE_INSTANCE_METHOD(void, set_ShowHandle, bool value);

    DECLARE_INSTANCE_FIELD(bool, _highlightHandle);
    DECLARE_INSTANCE_METHOD(bool, get_HighlightHandle);
    DECLARE_INSTANCE_METHOD(void, set_HighlightHandle, bool value);

    DECLARE_INSTANCE_FIELD(Side, _handleSide);
    DECLARE_INSTANCE_METHOD(Side, get_HandleSide);
    DECLARE_INSTANCE_METHOD(void, set_HandleSide, Side value);

    DECLARE_INSTANCE_METHOD(void, OnDestroy);
    DECLARE_INSTANCE_METHOD(void, UpdateHandle);
    DECLARE_DEFAULT_CTOR();
    public:
        UnorderedEventCallback<FloatingScreen*, const FloatingScreenHandleEventArgs&> HandleReleased;
        UnorderedEventCallback<FloatingScreen*, const FloatingScreenHandleEventArgs&> HandleGrabbed;
        
        template<typename T>
        requires(std::is_convertible_v<T, HMUI::ViewController*>)
        static FloatingScreen* CreateFloatingScreenWithViewcontroller(UnityEngine::Vector2 screenSize, bool createHandle, UnityEngine::Vector3 position, UnityEngine::Quaternion rotation, float curvatureRadius = 0.0f, bool hasBackground = false) {
            return CreateFloatingScreenWithViewcontroller(csTypeOf(T), screenSize, createHandle, position, rotation, curvatureRadius, hasBackground);
        }
        static FloatingScreen* CreateFloatingScreen(UnityEngine::Vector2 screenSize, bool createHandle, UnityEngine::Vector3 position, UnityEngine::Quaternion rotation, float curvatureRadius = 0.0f, bool hasBackground = false);
    private:
        void CreateHandle(VRUIControls::VRPointer* pointer = nullptr);
        static FloatingScreen* CreateFloatingScreenWithViewcontroller(Il2CppReflectionType* vcType, UnityEngine::Vector2 screenSize, bool createHandle, UnityEngine::Vector3 position, UnityEngine::Quaternion rotation, float curvatureRadius = 0.0f, bool hasBackground = false);
        static SafePtrUnity<UnityEngine::Material> _fogMaterial;
};